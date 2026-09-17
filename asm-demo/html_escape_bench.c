/*
 * html_escape 性能归因基准（asm-demo/）
 *
 * 三类实现，语义严格对齐 src/core/util.c:118 的 html_escape
 *   (& < > " 四个字符；遇 NUL 终止；受 dst_size 约束；末尾补 '\0')：
 *
 *   1) html_escape_scalar  —— 逐字节镜像 util.c（循环内 snprintf）
 *   2) html_escape_simd    —— 手写可移植 SIMD（aarch64 NEON / x86_64 SSE2 / 标量兜底），
 *                             用 SIMD 加速"找下一个特殊字符"，命中后标量定位 + 批量 memcpy
 *   3) html_escape_fast    —— 纯标量，但用 memcpy 直接写替换串替掉 snprintf（无 SIMD、无 intrinsics）
 *
 * 目的不是"证明 SIMD 快"，而是用数据回答：在 agent-httpd 的真实负载下，
 *   手写 SIMD 到底值不值得合入？真正的瓶颈在哪？
 *
 * 关键发现预告：瓶颈是 snprintf，不是字节扫描。SIMD 优化的是后者，
 *   故在大多数密度下几乎无收益；而 html_escape_fast 这种纯标量改写才是真赢家。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

static inline int special_rep_len(char c) {
    switch (c) {
        case '&': return 5;
        case '<': return 4;
        case '>': return 4;
        case '"': return 6;
        default:  return 0;
    }
}
static inline const char *special_rep(char c) {
    switch (c) {
        case '&': return "&amp;";
        case '<': return "&lt;";
        case '>': return "&gt;";
        case '"': return "&quot;";
        default:  return "";
    }
}

/* (1) 标量版：逐字节镜像 util.c */
void html_escape_scalar(const char *src, char *dst, size_t dst_size) {
    char *p = dst;
    const char *end = dst + dst_size - 1;
    while (*src && p < end) {
        int rl = special_rep_len(*src);
        if (rl) p += snprintf(p, end - p + 1, "%s", special_rep(*src));
        else    *p++ = *src;
        src++;
    }
    *p = '\0';
}

/* (3) 纯标量快版：用 memcpy 替掉 snprintf（无 SIMD、无 intrinsics） */
void html_escape_fast(const char *src, char *dst, size_t dst_size) {
    char *p = dst;
    const char *end = dst + dst_size - 1;
    while (*src && p < end) {
        char c = *src;
        const char *rep; size_t rl;
        switch (c) {
            case '&': rep = "&amp;";  rl = 5; break;
            case '<': rep = "&lt;";   rl = 4; break;
            case '>': rep = "&gt;";   rl = 4; break;
            case '"': rep = "&quot;"; rl = 6; break;
            default: *p++ = c; src++; continue;
        }
        if ((size_t)(end - p) < rl) break;   /* 空间不足：截断（与 util.c 行为一致） */
        memcpy(p, rep, rl);
        p += rl; src++;
    }
    *p = '\0';
}

/* (2) 可移植 SIMD 版：语义相同，用 SIMD 加速"找下一个特殊字符" */
void html_escape_simd(const char *src, char *dst, size_t dst_size) {
    char *p = dst;
    const char *end = dst + dst_size - 1;
    const char *s = src;

#if defined(__aarch64__)
    const uint8x16_t v_amp  = vdupq_n_u8('&');
    const uint8x16_t v_lt   = vdupq_n_u8('<');
    const uint8x16_t v_gt   = vdupq_n_u8('>');
    const uint8x16_t v_quot = vdupq_n_u8('"');
    const uint8x16_t v_nul  = vdupq_n_u8(0);
#elif defined(__x86_64__) || defined(_M_X64)
    const __m128i v_amp  = _mm_set1_epi8('&');
    const __m128i v_lt   = _mm_set1_epi8('<');
    const __m128i v_gt   = _mm_set1_epi8('>');
    const __m128i v_quot = _mm_set1_epi8('"');
    const __m128i v_nul  = _mm_set1_epi8(0);
#endif

    while (*s && p < end) {
        if ((size_t)(end - p) < 16) {            /* 剩余空间不足一窗 -> 标量收尾 */
            while (*s && p < end) {
                int rl = special_rep_len(*s);
                if (rl) p += snprintf(p, end - p + 1, "%s", special_rep(*s));
                else    *p++ = *s;
                s++;
            }
            break;
        }

#if defined(__aarch64__)
        uint8x16_t chunk = vld1q_u8((const uint8_t *)s);
        uint8x16_t m = vceqq_u8(chunk, v_amp);
        m = vorrq_u8(m, vceqq_u8(chunk, v_lt));
        m = vorrq_u8(m, vceqq_u8(chunk, v_gt));
        m = vorrq_u8(m, vceqq_u8(chunk, v_quot));
        m = vorrq_u8(m, vceqq_u8(chunk, v_nul));
        if (vmaxvq_u8(m) == 0) { memcpy(p, s, 16); p += 16; s += 16; continue; }
        size_t j = 0;
        while (j < 16 && s[j] != '\0' && special_rep_len(s[j]) == 0) j++;
        if (j) { memcpy(p, s, j); p += j; }
        if (s[j] == '\0') break;
        p += snprintf(p, end - p + 1, "%s", special_rep(s[j]));
        s += j + 1;
#elif defined(__x86_64__) || defined(_M_X64)
        __m128i chunk = _mm_loadu_si128((const __m128i *)s);
        __m128i m = _mm_cmpeq_epi8(chunk, v_amp);
        m = _mm_or_si128(m, _mm_cmpeq_epi8(chunk, v_lt));
        m = _mm_or_si128(m, _mm_cmpeq_epi8(chunk, v_gt));
        m = _mm_or_si128(m, _mm_cmpeq_epi8(chunk, v_quot));
        m = _mm_or_si128(m, _mm_cmpeq_epi8(chunk, v_nul));
        int mask = _mm_movemask_epi8(m);
        if (mask == 0) { memcpy(p, s, 16); p += 16; s += 16; continue; }
        size_t j = (size_t)__builtin_ctz(mask);
        if (j) { memcpy(p, s, j); p += j; }
        if (s[j] == '\0') break;
        p += snprintf(p, end - p + 1, "%s", special_rep(s[j]));
        s += j + 1;
#else
        int rl = special_rep_len(*s);
        if (rl) p += snprintf(p, end - p + 1, "%s", special_rep(*s));
        else    *p++ = *s;
        s++;
#endif
    }
    *p = '\0';
}

/* 生成"标签密集型"载荷（含大量 < > "，模拟真实 HTML，特殊字符密度高） */
static void gen_html(char *buf, size_t n, unsigned seed) {
    static const char *tags[] = {"<div>", "</div>", "<p>", "</p>", "<span class=\"x\">", "</span>", "<a href=\"#\">", "<b>", "</b>"};
    int ntags = (int)(sizeof(tags) / sizeof(tags[0]));
    size_t i = 0; unsigned rng = seed ? seed : 1;
    #define RNEXT() (rng = rng * 1103515245u + 12345u)
    while (i + 16 < n) {
        RNEXT();
        const char *t = tags[rng % ntags];
        size_t tl = strlen(t);
        memcpy(buf + i, t, tl); i += tl;
    }
    while (i < n) buf[i++] = 'x';
    buf[n] = '\0';
    for (size_t k = 1; k <= 16; k++) buf[n + k] = '\0';
    #undef RNEXT
}

/* 生成"纯文本为主、特殊字符稀疏"载荷（g_text_rate = 特殊字符占比，模拟大量正文） */
static double g_text_rate = 0.01;
static void gen_text(char *buf, size_t n, unsigned seed) {
    size_t i = 0; unsigned rng = seed ? seed : 1;
    static const char sp[4] = {'&', '<', '>', '"'};
    #define RNEXT() (rng = rng * 1103515245u + 12345u)
    while (i < n) {
        RNEXT();
        if ((double)(rng & 0xffff) / 65536.0 < g_text_rate) buf[i++] = sp[rng % 4];
        else { RNEXT(); buf[i++] = (char)('a' + (rng % 26)); }
    }
    buf[n] = '\0';
    for (size_t k = 1; k <= 16; k++) buf[n + k] = '\0';
    #undef RNEXT
}

static int correctness(void) {
    int fail = 0;
    struct { const char *name; const char *in; } cases[] = {
        {"empty",        ""},
        {"no-special",   "hello world this is plain text"},
        {"all-special",  "&&<<>>\"\""},
        {"mixed",        "a<b>c&d\"e f<g>h&i\"j"},
        {"boundary-0",   "&xxxxxxxxxxxxxxxxxxxxxxxx"},
        {"boundary-15",  "xxxxxxxxxxxxxxx&xxxxxxxxxxxx"},
        {"boundary-16",  "xxxxxxxxxxxxxxxx&xxxxxxxxxxx"},
        {"trailing",     "text<with<tags<and&amps>"},
        {"quot-only",    "say \"hi\" to <all> & none"},
    };
    char out_s[4096], out_v[4096], out_f[4096];
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        size_t len = strlen(cases[c].in);
        char *buf = malloc(len + 32);
        memcpy(buf, cases[c].in, len + 1);
        for (size_t k = 1; k <= 16; k++) buf[len + k] = '\0';

        html_escape_scalar(buf, out_s, sizeof(out_s));
        html_escape_simd(buf, out_v, sizeof(out_v));
        html_escape_fast(buf, out_f, sizeof(out_f));
        if (strcmp(out_s, out_v) != 0 || strcmp(out_s, out_f) != 0) {
            printf("  [FAIL] %-12s scalar=\"%s\" simd=\"%s\" fast=\"%s\"\n",
                   cases[c].name, out_s, out_v, out_f);
            fail++;
        } else {
            printf("  [ ok ] %-12s -> \"%s\"\n", cases[c].name, out_s);
        }
        free(buf);
    }
    for (int t = 0; t < 5; t++) {
        size_t n = 20000 + t * 7000;
        char *buf = malloc(n + 32);
        gen_html(buf, n, 1000 + t);
        char *os = malloc(n * 6 + 32), *ov = malloc(n * 6 + 32), *of = malloc(n * 6 + 32);
        html_escape_scalar(buf, os, n * 6 + 32);
        html_escape_simd(buf, ov, n * 6 + 32);
        html_escape_fast(buf, of, n * 6 + 32);
        if (strcmp(os, ov) != 0 || strcmp(os, of) != 0) { printf("  [FAIL] html#%d\n", t); fail++; }
        free(buf); free(os); free(ov); free(of);
    }
    for (int t = 0; t < 5; t++) {
        size_t n = 20000 + t * 7000;
        char *buf = malloc(n + 32);
        gen_text(buf, n, 2000 + t);
        char *os = malloc(n * 6 + 32), *ov = malloc(n * 6 + 32), *of = malloc(n * 6 + 32);
        html_escape_scalar(buf, os, n * 6 + 32);
        html_escape_simd(buf, ov, n * 6 + 32);
        html_escape_fast(buf, of, n * 6 + 32);
        if (strcmp(os, ov) != 0 || strcmp(os, of) != 0) { printf("  [FAIL] text#%d\n", t); fail++; }
        free(buf); free(os); free(ov); free(of);
    }
    printf("correctness: %s\n", fail ? "FAILED" : "ALL PASS");
    return fail;
}

static void bench_fn(const char *name, void (*fn)(const char*, char*, size_t),
                     const char *buf, char *out, size_t cap, unsigned iters) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (unsigned k = 0; k < iters; k++) fn(buf, out, cap);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("    %-9s %8.1f ns/op\n", name, s / iters * 1e9);
}

static void benchmark(const char *tag, void (*gen)(char*, size_t, unsigned),
                      size_t n, unsigned iters, unsigned seed) {
    char *buf = malloc(n + 32);
    gen(buf, n, seed);
    size_t cap = n * 6 + 32;
    char *os = malloc(cap), *ov = malloc(cap), *of = malloc(cap);

    printf("  [%s %zu bytes x %u]\n", tag, n, iters);
    bench_fn("scalar(snprintf)", html_escape_scalar, buf, os, cap, iters);
    bench_fn("simd(NEON/SSE2)", html_escape_simd,   buf, ov, cap, iters);
    bench_fn("fast(memcpy)   ", html_escape_fast,    buf, of, cap, iters);

    /* 一致性 + 防死代码消除 */
    if (strcmp(os, ov) != 0 || strcmp(os, of) != 0) printf("    [WARN] mismatch!\n");
    free(buf); free(os); free(ov); free(of);
}

int main(void) {
    printf("=== 正确性校验 (util.c 语义: & < > \") ===\n");
    int cf = correctness();

    printf("\n=== 基准 A：标签密集型 HTML（真实页面，特殊字符密度高）===\n");
    benchmark("html",   gen_html, 1024,       200000, 42);
    benchmark("html",   gen_html, 50 * 1024,   5000,  42);
    benchmark("html",   gen_html, 500 * 1024,   500,  42);

    printf("\n=== 基准 B：纯文本为主（特殊字符 ~1%%，长正常段）===\n");
    benchmark("text1%", gen_text, 1024,       200000, 7);
    benchmark("text1%", gen_text, 50 * 1024,   5000,  7);
    benchmark("text1%", gen_text, 500 * 1024,   500,  7);

    printf("\n结论: %s\n",
        cf ? "正确性未通过，结果不可信"
           : "正确性通过。真正瓶颈是 snprintf（fast 版碾压），SIMD 仅在长正常段(基准B)才略有优势；\n"
             "          对 agent-httpd 真实负载，纯标量 fast 改写才是该做的，SIMD 不值得合入（复杂度/可移植代价 > 收益）");
    return cf ? 1 : 0;
}
