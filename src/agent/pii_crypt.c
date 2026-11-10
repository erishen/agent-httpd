/* Framework-layer PII field encryption — see pii_crypt.h for the contract.
 *
 * The cipher is written fresh here (no OpenSSL, matching this tree's
 * zero-dependency rule) and pinned to RFC 8439 §2.8.2 by
 * tests/test_pii_crypt.c. ChaCha20-Poly1305 was chosen over AES-GCM on
 * purpose: no AES-NI/CLMUL dependency, no table lookups, and the spec has
 * published vectors.
 */

#include "pii_crypt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#endif

#define PII_MAX_RULES 32  /* PII_MAX_COLS comes from the header */

/* ===================== ChaCha20 (RFC 8439 §2.3) ===================== */

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static uint32_t load32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void store32le(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void qr(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    *a += *b; *d ^= *a; *d = ROTL32(*d, 16);
    *c += *d; *b ^= *c; *b = ROTL32(*b, 12);
    *a += *b; *d ^= *a; *d = ROTL32(*d, 8);
    *c += *d; *b ^= *c; *b = ROTL32(*b, 7);
}

static void chacha20_block(const unsigned char key[32], uint32_t counter,
                           const unsigned char nonce[12],
                           unsigned char out[64]) {
    uint32_t st[16], x[16];
    st[0] = 0x61707865; st[1] = 0x3320646e;
    st[2] = 0x79622d32; st[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) st[4 + i] = load32le(key + 4 * i);
    st[12] = counter;
    st[13] = load32le(nonce);
    st[14] = load32le(nonce + 4);
    st[15] = load32le(nonce + 8);
    memcpy(x, st, sizeof x);

    for (int i = 0; i < 10; i++) {
        qr(&x[0], &x[4], &x[8], &x[12]);
        qr(&x[1], &x[5], &x[9], &x[13]);
        qr(&x[2], &x[6], &x[10], &x[14]);
        qr(&x[3], &x[7], &x[11], &x[15]);
        qr(&x[0], &x[5], &x[10], &x[15]);
        qr(&x[1], &x[6], &x[11], &x[12]);
        qr(&x[2], &x[7], &x[8], &x[13]);
        qr(&x[3], &x[4], &x[9], &x[14]);
    }
    for (int i = 0; i < 16; i++) store32le(out + 4 * i, x[i] + st[i]);
}

static void chacha20_xor(const unsigned char key[32], uint32_t counter,
                         const unsigned char nonce[12],
                         const unsigned char *in, size_t len,
                         unsigned char *out) {
    unsigned char block[64];
    size_t off = 0;
    while (off < len) {
        chacha20_block(key, counter++, nonce, block);
        size_t n = len - off < 64 ? len - off : 64;
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ block[i];
        off += n;
    }
}

/* ===================== Poly1305 (RFC 8439 §2.5) ===================== */
/* 5x26-bit limb form (poly1305-donna-32 shape), 32-bit platform safe. */

typedef struct {
    uint32_t r[5], h[5], pad[4];
    unsigned char buf[16];
    size_t leftover;
} poly1305_ctx;

static void poly1305_blocks(poly1305_ctx *ctx, const unsigned char *m,
                            size_t bytes) {
    const uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2],
                   r3 = ctx->r[3], r4 = ctx->r[4];
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3],
             h4 = ctx->h[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

    while (bytes >= 16) {
        uint32_t t0 = load32le(m), t1 = load32le(m + 4);
        uint32_t t2 = load32le(m + 8), t3 = load32le(m + 12);

        h0 += t0 & 0x3ffffff;
        h1 += ((t0 >> 26) | (t1 << 6)) & 0x3ffffff;
        h2 += ((t1 >> 20) | (t2 << 12)) & 0x3ffffff;
        h3 += ((t2 >> 14) | (t3 << 18)) & 0x3ffffff;
        h4 += (t3 >> 8) | (1U << 24);

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 +
                      (uint64_t)h2 * s3 + (uint64_t)h3 * s2 +
                      (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 +
                      (uint64_t)h2 * s4 + (uint64_t)h3 * s3 +
                      (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 +
                      (uint64_t)h2 * r0 + (uint64_t)h3 * s4 +
                      (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 +
                      (uint64_t)h2 * r1 + (uint64_t)h3 * r0 +
                      (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 +
                      (uint64_t)h2 * r2 + (uint64_t)h3 * r1 +
                      (uint64_t)h4 * r0;

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = (h0 >> 26); h0 &= 0x3ffffff; h1 += c;

        m += 16;
        bytes -= 16;
    }

    ctx->h[0] = h0; ctx->h[1] = h1; ctx->h[2] = h2;
    ctx->h[3] = h3; ctx->h[4] = h4;
}

static void poly1305_init(poly1305_ctx *ctx, const unsigned char key[32]) {
    uint32_t t0 = load32le(key + 0), t1 = load32le(key + 4);
    uint32_t t2 = load32le(key + 8), t3 = load32le(key + 12);

    /* Clamp r through the limb masks (RFC 8439 §2.5.2). */
    ctx->r[0] = t0 & 0x3ffffff;
    ctx->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    ctx->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    ctx->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    ctx->r[4] = (t3 >> 8) & 0x00fffff;

    ctx->h[0] = ctx->h[1] = ctx->h[2] = ctx->h[3] = ctx->h[4] = 0;
    ctx->pad[0] = load32le(key + 16);
    ctx->pad[1] = load32le(key + 20);
    ctx->pad[2] = load32le(key + 24);
    ctx->pad[3] = load32le(key + 28);
    ctx->leftover = 0;
}

static void poly1305_update(poly1305_ctx *ctx, const unsigned char *m,
                            size_t bytes) {
    if (ctx->leftover) {
        size_t want = 16 - ctx->leftover;
        if (want > bytes) want = bytes;
        memcpy(ctx->buf + ctx->leftover, m, want);
        ctx->leftover += want;
        m += want;
        bytes -= want;
        if (ctx->leftover < 16) return;
        poly1305_blocks(ctx, ctx->buf, 16);
        ctx->leftover = 0;
    }
    if (bytes >= 16) {
        size_t want = bytes & (size_t)~(size_t)15;
        poly1305_blocks(ctx, m, want);
        m += want;
        bytes -= want;
    }
    if (bytes) {
        memcpy(ctx->buf + ctx->leftover, m, bytes);
        ctx->leftover += bytes;
    }
}

static void poly1305_finish(poly1305_ctx *ctx, unsigned char mac[16]) {
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3],
             h4 = ctx->h[4];

    if (ctx->leftover) {
        ctx->buf[ctx->leftover] = 1;
        memset(ctx->buf + ctx->leftover + 1, 0, 16 - ctx->leftover - 1);
        poly1305_blocks(ctx, ctx->buf, 16);
    }

    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    /* g = h + 5 - 2^130, then pick g when h >= p. */
    uint32_t g0 = h0 + 5, g1, g2, g3, g4;
    c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - ((uint32_t)1 << 26);
    uint32_t mask = (g4 >> 31) - 1;
    g4 &= mask; g3 &= mask; g2 &= mask; g1 &= mask; g0 &= mask;

    h0 = (h0 & ~mask) | g0;
    h1 = (h1 & ~mask) | g1;
    h2 = (h2 & ~mask) | g2;
    h3 = (h3 & ~mask) | g3;
    h4 = (h4 & ~mask) | g4;

    /* h % 2^128 */
    h0 = (h0 | (h1 << 26)) & 0xffffffffU;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffU;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffU;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffffU;

    uint64_t f;
    f = (uint64_t)h0 + ctx->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + ctx->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + ctx->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + ctx->pad[3] + (f >> 32); h3 = (uint32_t)f;

    store32le(mac + 0, h0);
    store32le(mac + 4, h1);
    store32le(mac + 8, h2);
    store32le(mac + 12, h3);
}

/* ===================== AEAD (RFC 8439 §2.8) ===================== */

static void aead_mac(const unsigned char key[32], const unsigned char nonce[12],
                     const unsigned char *aad, size_t aadlen,
                     const unsigned char *ct, size_t ctlen,
                     unsigned char tag[16]) {
    unsigned char polykey[64];
    chacha20_block(key, 0, nonce, polykey);

    poly1305_ctx ctx;
    poly1305_init(&ctx, polykey);

    static const unsigned char zeros[16] = {0};
    poly1305_update(&ctx, aad, aadlen);
    if (aadlen % 16) poly1305_update(&ctx, zeros, 16 - aadlen % 16);
    poly1305_update(&ctx, ct, ctlen);
    if (ctlen % 16) poly1305_update(&ctx, zeros, 16 - ctlen % 16);

    unsigned char lens[8];
    uint64_t v = (uint64_t)aadlen;
    for (int i = 0; i < 8; i++) lens[i] = (unsigned char)(v >> (8 * i));
    poly1305_update(&ctx, lens, 8);
    v = (uint64_t)ctlen;
    for (int i = 0; i < 8; i++) lens[i] = (unsigned char)(v >> (8 * i));
    poly1305_update(&ctx, lens, 8);

    poly1305_finish(&ctx, tag);
}

int pii_aead_seal(const unsigned char key[32], const unsigned char nonce[12],
                  const unsigned char *aad, size_t aadlen,
                  const unsigned char *pt, size_t ptlen,
                  unsigned char *ct, unsigned char tag[16]) {
    if (!key || !nonce || !tag) return -1;
    if (!pt && ptlen) return -1;
    if (!ct && ptlen) return -1;

    chacha20_xor(key, 1, nonce, pt, ptlen, ct);
    aead_mac(key, nonce, aad ? aad : (const unsigned char *)"",
             aad ? aadlen : 0, ct, ptlen, tag);
    return 0;
}

int pii_aead_open(const unsigned char key[32], const unsigned char nonce[12],
                  const unsigned char *aad, size_t aadlen,
                  const unsigned char *ct, size_t ctlen,
                  const unsigned char tag[16], unsigned char *pt) {
    if (!key || !nonce || !tag) return -1;
    if (!ct && ctlen) return -1;
    if (!pt && ctlen) return -1;

    unsigned char want[16];
    aead_mac(key, nonce, aad ? aad : (const unsigned char *)"",
             aad ? aadlen : 0, ct ? ct : (const unsigned char *)"", ctlen, want);

    /* Constant-time tag compare: a memcmp here leaks how many bytes match. */
    unsigned char diff = 0;
    for (int i = 0; i < 16; i++) diff |= (unsigned char)(want[i] ^ tag[i]);
    if (diff) return -1;

    chacha20_xor(key, 1, nonce, ct, ctlen, pt);
    return 0;
}

/* ===================== base64url ===================== */

static const char B64U[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char *b64url_encode(const unsigned char *in, size_t len) {
    char *out = malloc(4 * ((len + 2) / 3) + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        int rem = (int)(len - i);
        if (rem > 1) v |= (unsigned)in[i + 1] << 8;
        if (rem > 2) v |= in[i + 2];
        out[o++] = B64U[(v >> 18) & 0x3f];
        out[o++] = B64U[(v >> 12) & 0x3f];
        if (rem > 1) out[o++] = B64U[(v >> 6) & 0x3f];
        if (rem > 2) out[o++] = B64U[v & 0x3f];
    }
    out[o] = '\0';
    return out;
}

static int b64url_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* Returns 0 on success and mallocs `*out` (caller frees). */
static int b64url_decode(const char *in, unsigned char **out, size_t *outlen) {
    size_t len = strlen(in);
    if (len % 4 == 1) return -1;
    size_t cap = (len / 4) * 3 + 3;
    unsigned char *buf = malloc(cap ? cap : 1);
    if (!buf) return -1;
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64url_val((unsigned char)in[i]);
        if (v < 0) { free(buf); return -1; }
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[o++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    *out = buf;
    *outlen = o;
    return 0;
}

/* ===================== CSPRNG nonce ===================== */

static int fill_random(unsigned char *buf, size_t len) {
#if defined(__APPLE__)
    arc4random_buf(buf, len);
    return 0;
#elif defined(__linux__)
    size_t off = 0;
    while (off < len) {
        ssize_t n = getrandom(buf + off, len - off, 0);
        if (n < 0) return -1;
        off += (size_t)n;
    }
    return 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
#endif
}

/* ===================== policy ===================== */

typedef struct {
    char table[PII_MAX_COLS];
    char column[PII_MAX_COLS];
} pii_rule;

static pii_rule g_rules[PII_MAX_RULES];
static int g_nrules;
static unsigned char g_key[32], g_prev[32];
static char g_key_id[24], g_prev_id[24];
static int g_have_key, g_have_prev;
static int g_env_loaded;

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static void parse_key(const char *hex, unsigned char out[32]) {
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

static int key_hex_ok(const char *hex) {
    if (!hex || strlen(hex) != 64) return 0;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)hex[i])) return 0;
    return 1;
}

void pii_policy_reset(const char *columns_csv, const char *secret_hex,
                      const char *key_id) {
    g_env_loaded = 1; /* explicit policy wins over LUME_PII_COLUMNS */
    g_nrules = 0;
    g_have_key = g_have_prev = 0;
    g_key_id[0] = g_prev_id[0] = '\0';

    if (key_hex_ok(secret_hex)) {
        parse_key(secret_hex, g_key);
        g_have_key = 1;
        snprintf(g_key_id, sizeof g_key_id, "%s",
                 key_id && key_id[0] ? key_id : "1");
    }

    if (!columns_csv || !columns_csv[0]) return;

    char csv[512];
    snprintf(csv, sizeof csv, "%s", columns_csv);
    char *save = NULL;
    for (char *tok = strtok_r(csv, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        if (g_nrules >= PII_MAX_RULES) break;
        trim(tok);
        char *dot = strchr(tok, '.');
        if (!dot || dot == tok || !dot[1]) continue;
        *dot = '\0';
        snprintf(g_rules[g_nrules].table, PII_MAX_COLS, "%s", tok);
        snprintf(g_rules[g_nrules].column, PII_MAX_COLS, "%s", dot + 1);
        g_nrules++;
    }
}

void pii_policy_set_previous(const char *key_id, const char *secret_hex) {
    if (!key_hex_ok(secret_hex) || !key_id || !key_id[0]) return;
    parse_key(secret_hex, g_prev);
    snprintf(g_prev_id, sizeof g_prev_id, "%s", key_id);
    g_have_prev = 1;
}

void pii_policy_from_env(void) {
    if (g_env_loaded) return;
    g_env_loaded = 1;
    const char *cols = getenv("LUME_PII_COLUMNS");
    if (!cols || !cols[0]) return;
    pii_policy_reset(cols, getenv("LUME_PII_SECRET"), getenv("LUME_PII_KEY_ID"));
    const char *pv = getenv("LUME_PII_SECRET_PREV");
    if (pv && pv[0]) pii_policy_set_previous(getenv("LUME_PII_KEY_ID_PREV"), pv);
}

int pii_configured(void) {
    pii_policy_from_env();
    return g_nrules > 0;
}

int pii_is_column(const char *table, const char *column) {
    if (!table || !column) return 0;
    /* The read path asks this question column by column and may never touch
     * pii_configured() first — a process that only reads would otherwise
     * decrypt nothing. Any explicit pii_policy_reset() marks the policy as
     * decided, so tests keep their own rules. */
    pii_policy_from_env();
    for (int i = 0; i < g_nrules; i++)
        if (strcmp(g_rules[i].table, table) == 0 &&
            strcmp(g_rules[i].column, column) == 0) return 1;
    return 0;
}

/* ===================== value transforms ===================== */

static void set_err(char *err, size_t errsz, const char *msg) {
    if (err && errsz) snprintf(err, errsz, "%s", msg);
}

char *pii_encrypt_value(const char *table, const char *column, const char *plain,
                        char *err, size_t errsz) {
    if (err && errsz) err[0] = '\0';
    if (!pii_configured()) return NULL;
    if (!pii_is_column(table, column)) return NULL;
    if (!g_have_key) {
        set_err(err, errsz, "pii: LUME_PII_SECRET missing or not 32-byte hex");
        return NULL;
    }
    if (!plain) {
        set_err(err, errsz, "pii: NULL value cannot be encrypted");
        return NULL;
    }
    /* Already a token (re-encrypt of a decrypted row, or a duplicated
     * write): leave it alone rather than double-wrapping. */
    if (strncmp(plain, PII_TOKEN_PREFIX, strlen(PII_TOKEN_PREFIX)) == 0)
        return strdup(plain);

    char aad[PII_MAX_COLS * 2 + 2];
    snprintf(aad, sizeof aad, "%s:%s", table, column);

    size_t ptlen = strlen(plain);
    unsigned char nonce[12];
    if (fill_random(nonce, sizeof nonce) != 0) {
        set_err(err, errsz, "pii: CSPRNG unavailable");
        return NULL;
    }
    unsigned char *ct = malloc(ptlen ? ptlen : 1);
    unsigned char tag[16];
    if (!ct) { set_err(err, errsz, "pii: out of memory"); return NULL; }
    if (pii_aead_seal(g_key, nonce, (const unsigned char *)aad, strlen(aad),
                      (const unsigned char *)plain, ptlen, ct, tag) != 0) {
        free(ct);
        set_err(err, errsz, "pii: seal failed");
        return NULL;
    }

    /* nonce ‖ ciphertext ‖ tag, base64url'd */
    size_t rawlen = sizeof nonce + ptlen + sizeof tag;
    unsigned char *raw = malloc(rawlen);
    if (!raw) { free(ct); set_err(err, errsz, "pii: out of memory"); return NULL; }
    memcpy(raw, nonce, sizeof nonce);
    memcpy(raw + sizeof nonce, ct, ptlen);
    memcpy(raw + sizeof nonce + ptlen, tag, sizeof tag);
    free(ct);

    char *b64 = b64url_encode(raw, rawlen);
    free(raw);
    if (!b64) { set_err(err, errsz, "pii: out of memory"); return NULL; }

    size_t need = strlen(PII_TOKEN_PREFIX) + strlen(g_key_id) + strlen(b64) + 2;
    char *tok = malloc(need);
    if (!tok) { free(b64); set_err(err, errsz, "pii: out of memory"); return NULL; }
    snprintf(tok, need, "%s%s.%s", PII_TOKEN_PREFIX, g_key_id, b64);
    free(b64);
    return tok;
}

char *pii_decrypt_value(const char *table, const char *column, const char *token,
                        char *err, size_t errsz) {
    if (err && errsz) err[0] = '\0';
    if (!token) return NULL;
    /* Legacy plaintext / non-token value: return unchanged, never 500 and
     * never an empty string (crypto_pii.py's InvalidToken rule). */
    if (strncmp(token, PII_TOKEN_PREFIX, strlen(PII_TOKEN_PREFIX)) != 0)
        return strdup(token);
    if (!pii_is_column(table, column)) return strdup(token);

    const char *id = token + strlen(PII_TOKEN_PREFIX);
    const char *dot = strchr(id, '.');
    if (!dot) {
        set_err(err, errsz, "pii: malformed token (no key id)");
        return NULL;
    }
    char kid[24];
    size_t idlen = (size_t)(dot - id);
    if (idlen == 0 || idlen >= sizeof kid) {
        set_err(err, errsz, "pii: malformed token (bad key id)");
        return NULL;
    }
    memcpy(kid, id, idlen);
    kid[idlen] = '\0';

    const unsigned char *key = NULL;
    if (g_have_key && strcmp(kid, g_key_id) == 0) key = g_key;
    else if (g_have_prev && strcmp(kid, g_prev_id) == 0) key = g_prev;
    if (!key) {
        set_err(err, errsz, "pii: no key for this token's key-id");
        return NULL;
    }

    unsigned char *raw = NULL;
    size_t rawlen = 0;
    if (b64url_decode(dot + 1, &raw, &rawlen) != 0) {
        set_err(err, errsz, "pii: malformed token (bad base64)");
        return NULL;
    }
    if (rawlen < 12 + 16) {
        free(raw);
        set_err(err, errsz, "pii: token too short");
        return NULL;
    }
    size_t ctlen = rawlen - 12 - 16;
    char aad[PII_MAX_COLS * 2 + 2];
    snprintf(aad, sizeof aad, "%s:%s", table, column);

    unsigned char *pt = malloc(ctlen ? ctlen : 1);
    if (!pt) {
        free(raw);
        set_err(err, errsz, "pii: out of memory");
        return NULL;
    }
    int rc = pii_aead_open(key, raw, (const unsigned char *)aad, strlen(aad),
                           raw + 12, ctlen, raw + 12 + ctlen, pt);
    free(raw);
    if (rc != 0) {
        free(pt);
        set_err(err, errsz, "pii: authentication failed (tampered or relocated)");
        return NULL;
    }
    char *out = malloc(ctlen + 1);
    if (!out) {
        free(pt);
        set_err(err, errsz, "pii: out of memory");
        return NULL;
    }
    memcpy(out, pt, ctlen);
    out[ctlen] = '\0';
    free(pt);
    return out;
}

/* ===================== SQL shape helpers ===================== */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Reads one SQL identifier (bare, `backtick`, "double-quote"). Returns the
 * position after it, or NULL when nothing valid is there. */
static const char *read_ident(const char *p, char *out, size_t outsz) {
    p = skip_ws(p);
    char q = 0;
    if (*p == '`' || *p == '"' || *p == '\'') q = *p++;
    size_t o = 0;
    while (*p) {
        if (q) {
            if (*p == q) { p++; break; }
        } else if (!(isalnum((unsigned char)*p) || *p == '_')) {
            break;
        }
        if (o + 1 < outsz) out[o++] = *p;
        p++;
    }
    if (outsz) out[o] = '\0';
    return o ? p : NULL;
}

static int ci_prefix(const char *s, const char *word) {
    size_t n = strlen(word);
    return strncasecmp(s, word, n) == 0 &&
           !(isalnum((unsigned char)s[n]) || s[n] == '_');
}

/* Parse `INSERT INTO <table> (<cols>) VALUES (<items>)`.
 *
 * An item is either a positional `?` (kind=1, remembering its parameter
 * index) or a literal (`0`, `'x'`, `NULL`, `CURRENT_TIMESTAMP`, ...)
 * (kind=0). Literal shapes are accepted but tracked, because a literal in a
 * PII column means the plaintext would bypass encryption — the caller must
 * refuse that, not quietly write it.
 *
 * Returns 0 on success, -1 when the statement is not a shape this parser can
 * attribute (no column list, named/:param placeholders, INTO ... SELECT). */
static int parse_insert(const char *sql, char *table, size_t table_sz,
                        char cols[][PII_MAX_COLS], int *is_param,
                        int *param_at, int max_cols, int *ncols) {
    const char *p = skip_ws(sql);
    if (!ci_prefix(p, "insert")) return -1;
    p = skip_ws(p + 6);
    if (!ci_prefix(p, "into")) return -1;
    p = skip_ws(p + 4);

    const char *after_table = read_ident(p, table, table_sz);
    if (!after_table) return -1;
    p = skip_ws(after_table);
    if (*p != '(') return -1; /* INSERT INTO t VALUES (...) — refused */
    p++;

    int n = 0;
    while (1) {
        p = skip_ws(p);
        if (*p == ')') { p++; break; }
        if (n >= max_cols) return -1;
        const char *next = read_ident(p, cols[n], PII_MAX_COLS);
        if (!next) return -1;
        n++;
        p = skip_ws(next);
        if (*p == ',') { p++; continue; }
        if (*p == ')') { p++; break; }
        return -1;
    }

    p = skip_ws(p);
    if (!ci_prefix(p, "values")) return -1;
    p = skip_ws(p + 6);
    if (*p != '(') return -1;
    p++;

    int col = 0, nparams = 0;
    while (1) {
        p = skip_ws(p);
        if (*p == ')') { p++; break; }
        if (col >= n) return -1;

        if (*p == '?') {
            is_param[col] = 1;
            param_at[col] = nparams++;
            p++;
        } else {
            /* A literal item runs until a top-level ',' or ')'. Quoted
             * strings and nested parentheses are skipped as units. */
            is_param[col] = 0;
            param_at[col] = -1;
            const char *lit_start = p;
            char q = 0;
            int depth = 0;
            while (*p) {
                if (q) {
                    if (*p == q) q = 0;
                    p++;
                    continue;
                }
                if (*p == '\'' || *p == '"') { q = *p++; continue; }
                if (*p == '(') { depth++; p++; continue; }
                if (*p == ')' && depth > 0) { depth--; p++; continue; }
                if (depth == 0 && (*p == ',' || *p == ')')) break;
                p++;
            }
            if (q || p == lit_start) return -1; /* unterminated / empty item */
        }
        col++;
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == ')') { p++; break; }
        return -1;
    }
    if (col != n) return -1;

    p = skip_ws(p);
    if (*p && *p != ';') return -1; /* trailing clause (ON CONFLICT etc.) */

    *ncols = n;
    return 0;
}

/* Parameter-index-only view of pii_insert_targets, kept because the pinned
 * unit test asserts the INSERT -> parameter map directly. */
int pii_insert_param_columns(const char *sql, int *idx, int max, int *n) {
    if (n) *n = 0;
    pii_target targets[PII_MAX_COLS];
    int found = 0;
    int rc = pii_insert_targets(sql, targets, PII_MAX_COLS, &found);
    if (rc != 0) return rc; /* -1 unparseable, -2 literal in a PII column */
    for (int i = 0; i < found; i++) {
        if (!idx || i >= max) return -1;
        idx[i] = targets[i].param;
    }
    if (n) *n = found;
    return 0;
}

int pii_table_has_pii(const char *table) {
    if (!table || !table[0]) return 0;
    for (int i = 0; i < g_nrules; i++)
        if (strcmp(g_rules[i].table, table) == 0) return 1;
    return 0;
}

int pii_insert_targets(const char *sql, pii_target *out, int max, int *n) {
    if (n) *n = 0;
    if (!sql || !pii_configured()) return -1;

    char table[PII_MAX_COLS] = "";
    static char cols[PII_MAX_COLS][PII_MAX_COLS];
    static int is_param[PII_MAX_COLS], param_at[PII_MAX_COLS];
    int ncols = 0;
    if (parse_insert(sql, table, sizeof table, cols, is_param, param_at,
                     PII_MAX_COLS, &ncols) != 0)
        return -1;

    int found = 0;
    for (int i = 0; i < ncols; i++) {
        if (!pii_is_column(table, cols[i])) continue;
        /* An inline literal in a PII column is plaintext that would bypass
         * encryption; the caller must refuse it. -2 says exactly that. */
        if (!is_param[i]) return -2;
        if (!out || found >= max) return -1;
        out[found].param = param_at[i];
        snprintf(out[found].column, PII_MAX_NAME, "%s", cols[i]);
        found++;
    }
    if (n) *n = found;
    return 0;
}

/* Deliberately narrower than parse_insert: the target table is known from
 * the statement head even when the value list cannot be attributed, and the
 * write hook needs that name to decide whether to fail closed. */
int pii_insert_table(const char *sql, char *table_out, size_t table_sz) {
    if (table_out && table_sz) table_out[0] = '\0';
    if (!sql) return -1;
    const char *p = skip_ws(sql);
    if (!ci_prefix(p, "insert")) return -1;
    p = skip_ws(p + 6);
    if (!ci_prefix(p, "into")) return -1;
    p = skip_ws(p + 4);
    char table[PII_MAX_COLS] = "";
    if (!read_ident(p, table, sizeof table)) return -1;
    if (table_out && table_sz) snprintf(table_out, table_sz, "%s", table);
    return 0;
}

void pii_select_table(const char *sql, char *table_out, size_t table_sz) {
    if (!table_out || !table_sz) return;
    table_out[0] = '\0';
    if (!sql) return;
    const char *p = skip_ws(sql);
    if (!ci_prefix(p, "select")) return;

    /* A join / union / subquery makes "which table owns this column"
     * ambiguous, so the read hook leaves the row untouched instead of
     * guessing a table for the AAD. */
    if (strcasestr(p, " join ") || strcasestr(p, " union ")) return;

    const char *from = NULL;
    for (const char *q = p; *q; q++) {
        if (ci_prefix(q, "from") && (q == p || isspace((unsigned char)q[-1]))) {
            from = q + 4;
            break;
        }
    }
    if (!from) return;
    char table[PII_MAX_COLS] = "";
    if (!read_ident(from, table, sizeof table)) return;
    snprintf(table_out, table_sz, "%s", table);
}
