/* memchr_bench.c — 对照 http.c chunked 解码里的三处
 *   for (; ls < have; ls++) if (buffer[ls] == '\n') break;
 * 这是一个干净的「单字节查找」原语（memchr 的形状）。
 *
 * 三种实现：
 *   scalar  —— 手工逐字节循环（与 http.c 写法一致）
 *   memchr  —— 直接调用 libc memchr（glibc/musl/Apple libc 内部已是 NEON/SSE2）
 *   simd    —— 手写可移植 SIMD（aarch64 NEON / x86_64 SSE2 / 标量兜底）
 *
 * 结论预告：手写 SIMD 能赢标量，但赢不了 libc memchr —— 因为 libc 的 memchr
 * 本身就是手写 SIMD 的精心调校版。所以这里「用汇编」的正确姿势是「用 memchr」，
 * 而非自己再写一遍。且本服务器 body 封顶 64KB、IO 主导，绝对收益微乎其微。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__aarch64__)
#  include <arm_neon.h>
#elif defined(__x86_64__)
#  include <emmintrin.h>
#endif

/* ---------- 1) 标量：与 http.c 写法一致 ---------- */
static long scan_newline_scalar(const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (buf[i] == '\n') return (long)i;
    return -1;
}

/* ---------- 2) libc memchr ---------- */
static long scan_newline_memchr(const char *buf, size_t n) {
    const char *p = (const char *)memchr(buf, '\n', n);
    return p ? (long)(p - buf) : -1;
}

/* ---------- 3) 手写可移植 SIMD ---------- */
static long scan_newline_simd(const char *buf, size_t n) {
#if defined(__aarch64__)
    size_t i = 0;
    uint8x16_t vn = vdupq_n_u8('\n');
    for (; i + 16 <= n; i += 16) {
        uint8x16_t c  = vld1q_u8((const uint8_t *)buf + i);   /* 未对齐安全 */
        uint8x16_t eq = vceqq_u8(c, vn);
        if (vmaxvq_u8(eq)) {                                  /* 块内任一字节 =='\n' */
            for (size_t j = i; j < i + 16; j++)
                if (buf[j] == '\n') return (long)j;            /* 标量定位具体下标 */
        }
    }
    for (; i < n; i++) if (buf[i] == '\n') return (long)i;     /* 尾部 0-15 字节 */
    return -1;
#elif defined(__x86_64__)
    size_t i = 0;
    __m128i vn = _mm_set1_epi8('\n');
    for (; i + 16 <= n; i += 16) {
        __m128i c  = _mm_loadu_si128((const __m128i *)(buf + i)); /* 未对齐安全 */
        __m128i eq = _mm_cmpeq_epi8(c, vn);
        int mask = _mm_movemask_epi8(eq);
        if (mask) {
            for (size_t j = i; j < i + 16; j++)
                if (buf[j] == '\n') return (long)j;
        }
    }
    for (; i < n; i++) if (buf[i] == '\n') return (long)i;
    return -1;
#else
    return scan_newline_scalar(buf, n);   /* 其他架构：标量兜底 */
#endif
}

/* ---------- 正确性校验 ---------- */
static int verify(void) {
    enum { N = 4096 };
    char *buf = malloc(N);
    /* 全 'A'，仅在随机/边界位置放 '\n' */
    memset(buf, 'A', N);
    long pos[8] = {0, 1, 15, 16, 17, 1000, 4094, 4095};
    for (int t = 0; t < 8; t++) {
        buf[pos[t]] = '\n';
        long a = scan_newline_scalar(buf, N);
        long b = scan_newline_memchr(buf, N);
        long c = scan_newline_simd(buf, N);
        if (!(a == b && b == c && a == pos[t])) {
            printf("MISMATCH t=%d pos=%ld scalar=%ld memchr=%ld simd=%ld\n",
                   t, pos[t], a, b, c);
            free(buf);
            return 0;
        }
        buf[pos[t]] = 'A'; /* 复位 */
    }
    /* 无 '\n' 情形 */
    buf[0] = 'A';
    long a = scan_newline_scalar(buf, N);
    long b = scan_newline_memchr(buf, N);
    long c = scan_newline_simd(buf, N);
    if (!(a == b && b == c && a == -1)) {
        printf("MISMATCH none: scalar=%ld memchr=%ld simd=%ld\n", a, b, c);
        free(buf);
        return 0;
    }
    free(buf);
    return 1;
}

/* ---------- 基准 ---------- */
static double bench(long (*fn)(const char *, size_t), const char *buf, size_t n, int iters) {
    /* 预热 + 计最小时长，避免被编译器把循环优化掉 */
    volatile long sink = 0;
    long r = fn(buf, n); sink += r;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++) sink += fn(buf, n);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (void)sink;
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    return sec / iters * 1e9; /* ns/op */
}

static void gen(char *buf, size_t n, int pattern) {
    if (pattern == 0) {                       /* 全是 'A'，'\n' 在最后（最坏情况） */
        memset(buf, 'A', n);
        buf[n - 1] = '\n';
    } else if (pattern == 1) {                /* 每行 64 字节文本 + '\n'（真实 HTTP 风） */
        for (size_t i = 0; i < n; i++) buf[i] = (i % 64 == 63) ? '\n' : 'A';
    } else {                                  /* 随机数据 */
        unsigned rng = 12345u;
        for (size_t i = 0; i < n; i++) {
            rng = rng * 1103515245u + 12345u;
            buf[i] = (char)('@' + (rng & 0x3f));   /* 避开 '\n'(0x0a) 以免提前命中 */
            if (buf[i] == '\n') buf[i] = 'A';
        }
        buf[n - 1] = '\n';
    }
}

int main(void) {
    if (!verify()) { printf("CORRECTNESS FAIL\n"); return 1; }
    printf("CORRECTNESS: PASS (scalar == memchr == simd)\n\n");

    size_t sizes[] = { 1024, 65536, 1024*1024, 8*1024*1024 };
    const char *names[] = { "1KB", "64KB(=服务器上限)", "1MB", "8MB" };
    const char *pat_name[] = { "全A/'\\n'在尾(最坏)", "每行64B+'\\n'", "随机+尾部'\\n'" };
    int iters_for[] = { 200000, 20000, 1000, 200 };

    for (int p = 0; p < 3; p++) {
        printf("=== 模式: %s ===\n", pat_name[p]);
        for (int s = 0; s < 4; s++) {
            char *buf = malloc(sizes[s]);
            gen(buf, sizes[s], p);
            double t_scalar = bench(scan_newline_scalar, buf, sizes[s], iters_for[s]);
            double t_memchr  = bench(scan_newline_memchr,  buf, sizes[s], iters_for[s]);
            double t_simd    = bench(scan_newline_simd,    buf, sizes[s], iters_for[s]);
            printf("  %-16s scalar=%8.1fns  memchr=%8.1fns(x%.2f)  simd=%8.1fns(x%.2f)\n",
                   names[s], t_scalar, t_memchr, t_scalar/t_memchr,
                   t_simd, t_scalar/t_simd);
            free(buf);
        }
        printf("\n");
    }
    return 0;
}
