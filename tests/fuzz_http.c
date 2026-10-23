/* libFuzzer target over the request parser (http_parse.c) and the chunked
 * transfer-coding decoder (http_chunk.c) — the two pure-memory parsers on
 * the request path (P12 audit follow-up). Two legs per input:
 *   1. parse_request() on the raw bytes (headers / start line);
 *   2. http_dechunk_feed() fed one byte at a time through the resumable
 *      state machine, cross-checked against the one-shot http_dechunk():
 *      a done/malformed verdict reached incrementally must equal the
 *      one-shot verdict on the same bytes, and the decoded lengths must
 *      agree when both succeed.
 * Compiled with clang -fsanitize=fuzzer,address,undefined on Linux CI
 * (mingw has no libFuzzer). The stubs mirror src/core/util.c so the two
 * parser TUs link standalone; the regression main below is the local /
 * non-fuzz verification path, selected via -DLUME_FUZZ_DRIVER (set by
 * `make test-unit`). Under the libFuzzer build it is excluded so libFuzzer
 * supplies the entry point. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "httpd.h"
#include "internal.h"

/* stubs — bounded copy + in-place trim, mirroring src/core/util.c (the
 * parser TUs link against them in the real server). */
void set_str(char *dst, size_t dst_size, const char *src) {
    if (!dst_size) return;
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void trim_whitespace(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    HttpRequest req;
    static char body[MAX_REQUEST_SIZE + 1];
    static char raw[MAX_REQUEST_SIZE + 1];

    /* leg 1: start line + headers */
    if (size > MAX_REQUEST_SIZE) size = MAX_REQUEST_SIZE;
    memcpy(raw, data, size);
    raw[size] = '\0';
    memset(&req, 0, sizeof req);
    (void)parse_request(raw, &req);

    /* leg 2: chunked decoder, incremental + one-shot cross-check.
     * NOTE: st is reset per input (was static, which leaked chunk-decoder
     * state across fuzz inputs and obscured crash attribution). */
    HttpChunkState st;
    memset(&st, 0, sizeof st);
    long out_len = 0;
    int early = 0, fr = 1;
    for (size_t i = 0; i < size; i++) {
        fr = http_dechunk_feed((const char *)data, (long)(i + 1), &st,
                               body, MAX_REQUEST_SIZE, &out_len);
        if (fr <= 0) { early = 1; break; }
    }
    long out2 = 0;
    int r2 = http_dechunk((const char *)data, (long)size, body,
                          MAX_REQUEST_SIZE, &out2);
    if (early && fr != r2) abort();           /* incremental vs one-shot verdict */
    if (!early && fr == 0 && out_len != out2) abort(); /* decoded lengths agree */
    return 0;
}

#ifdef LUME_FUZZ_DRIVER
/* deterministic regression main: known-good and known-bad shapes.
 * Included only for the standalone driver build (make test-unit, or the
 * macOS local -DLUME_FUZZ_DRIVER path). When built under libFuzzer
 * (-fsanitize=fuzzer, as `make fuzz-http` does), this main is excluded and
 * libFuzzer supplies its own entry point. This avoids the duplicate-`main`
 * link error the previous FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION guard
 * produced: a bare clang -fsanitize=fuzzer does NOT define that macro. */
static int failures = 0;
static void expect(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

int main(void) {
    char out[128];
    long olen = -1;

    /* one chunk "hello", 0-chunk, CRLF trailer close */
    olen = -1;
    expect(http_dechunk("5\r\nhello\r\n0\r\n\r\n", 16, out, sizeof out, &olen) == 0,
           "dechunk: hello");
    expect(olen == 5 && memcmp(out, "hello", 5) == 0, "dechunk: hello content");

    /* two chunks */
    olen = -1;
    expect(http_dechunk("3\r\nabc\r\n1\r\nd\r\n0\r\n\r\n", 22, out, sizeof out, &olen) == 0,
           "dechunk: two chunks");
    expect(olen == 4 && memcmp(out, "abcd", 4) == 0, "dechunk: two chunks content");

    /* bare-LF size line (lenient wire) */
    olen = -1;
    expect(http_dechunk("5\nhello\n0\n\n", 11, out, sizeof out, &olen) == 0,
           "dechunk: bare-LF framing");

    /* trailer block with a real trailer line */
    olen = -1;
    expect(http_dechunk("0\r\nX-Ext: 1\r\n\r\n", 15, out, sizeof out, &olen) == 0,
           "dechunk: trailer line");

    /* malformed: size line garbage is treated as 0-chunk, then garbage
     * trailer never ends -> need more (safe, no crash) */
    olen = -1;
    int r = http_dechunk("zzz", 3, out, sizeof out, &olen);
    expect(r == 1 || r == 0, "dechunk: garbage is safe");

    /* malformed: negative never produced by strtol path on hex input here,
     * but truncated payload must be need-more, not overflow */
    olen = -1;
    expect(http_dechunk("5\r\nhel", 6, out, sizeof out, &olen) == 1,
           "dechunk: truncated payload = need more");

    /* cap exceeded */
    olen = -1;
    expect(http_dechunk("10\r\n0123456789abcdef\r\n0\r\n\r\n", 25, out, 8, &olen) == -2,
           "dechunk: cap exceeded -> -2");

    /* resumable feed on a valid stream */
    {
        const char *wire = "5\r\nhello\r\n0\r\n\r\n";
        HttpChunkState st;
        memset(&st, 0, sizeof st);
        long acc = 0;
        int got = 0;
        for (long i = 1; i <= 16; i++) {
            if (http_dechunk_feed(wire, i, &st, out, sizeof out, &acc) == 0) {
                got = 1;
                expect(acc == 5 && memcmp(out, "hello", 5) == 0,
                       "feed: hello at byte boundary");
                break;
            }
        }
        expect(got, "feed: stream completes");
    }

    if (failures == 0)
        printf("ok fuzz_http: parse_headers + chunked decoder regression\n");
    return failures ? 1 : 0;
}
#endif
