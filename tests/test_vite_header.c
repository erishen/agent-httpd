/* Unit test for vite_build_headers (extracted from http.c). Verifies the
 * dev-proxy header construction never writes past bufsz, strips hop-by-hop
 * headers, and appends Host/Connection. Compiled together with src/vite.c
 * under ASan+UBSan so any out-of-bounds read/write in vite.c is caught. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "httpd.h"

/* search needle within the first n bytes of hay (hay is not NUL-terminated) */
static int contains(const char *hay, size_t n, const char *needle) {
    size_t k = strlen(needle);
    if (k == 0 || k > n) return 0;
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(hay + i, needle, k) == 0) return 1;
    return 0;
}

static char *make_big_headers(size_t *out_len) {
    size_t cap = 128 * 1024;
    char *raw = malloc(cap);
    size_t off = 0;
    off += (size_t)snprintf(raw + off, cap - off, "GET /@vite/client HTTP/1.1\r\n");
    for (int i = 0; i < 400; i++) {
        char pad[224];
        memset(pad, '.', sizeof(pad));
        off += (size_t)snprintf(raw + off, cap - off, "X-Pad-%03d: %.*s\r\n", i,
                                (int)sizeof(pad), pad);
    }
    off += (size_t)snprintf(raw + off, cap - off, "\r\n");
    *out_len = off;
    return raw;
}

int main(void) {
    int failures = 0;

    /* 1) pathological 16KB+ header block must not overflow the 16KB buffer */
    {
        size_t raw_len;
        char *raw = make_big_headers(&raw_len);
        char buf[16384];
        size_t n = vite_build_headers(buf, sizeof(buf), raw, raw_len, 5173);
        if (n > sizeof(buf)) {
            fprintf(stderr, "FAIL: wrote %zu bytes into %zu-byte buffer\n", n, sizeof(buf));
            failures++;
        }
        const char *tail = "Host: 127.0.0.1:5173\r\nConnection: close\r\n\r\n";
        if (n < strlen(tail) || memcmp(buf + n - strlen(tail), tail, strlen(tail)) != 0) {
            fprintf(stderr, "FAIL: missing Host/Connection tail (n=%zu)\n", n);
            failures++;
        }
        printf("case1 oversized headers: wrote %zu bytes, no overflow\n", n);
        free(raw);
    }

    /* 2) normal request: client Host stripped+overridden, non-hop-by-hop kept */
    {
        const char *raw = "GET /foo HTTP/1.1\r\n"
                          "Host: evil.example\r\n"
                          "Content-Length: 0\r\n"
                          "X-Custom: keepme\r\n"
                          "\r\n";
        char buf[16384];
        size_t n = vite_build_headers(buf, sizeof(buf), raw, strlen(raw), 3000);
        if (n > sizeof(buf)) failures++;
        if (contains(buf, n, "evil.example")) {
            fprintf(stderr, "FAIL: client Host leaked to upstream\n");
            failures++;
        }
        if (!contains(buf, n, "X-Custom: keepme")) {
            fprintf(stderr, "FAIL: non-hop-by-hop header was dropped\n");
            failures++;
        }
        if (!contains(buf, n, "Host: 127.0.0.1:3000")) {
            fprintf(stderr, "FAIL: upstream Host not appended\n");
            failures++;
        }
        printf("case2 normal headers: n=%zu, Host overridden, custom kept\n", n);
    }

    /* 3) a header value carrying an embedded CR (before its own CRLF) must
     *    stop forwarding at that line - the caller would 400 and the rest of
     *    the block (including any injected "GET /admin") must not reach the
     *    upstream Vite. */
    {
        const char *raw = "GET /foo HTTP/1.1\r\n"
                          "X-Smuggle: a\r\r\n"   /* embedded CR before the line terminator */
                          "GET /admin HTTP/1.1\r\n"
                          "\r\n";
        char buf[16384];
        size_t n = vite_build_headers(buf, sizeof(buf), raw, strlen(raw), 3000);
        if (contains(buf, n, "GET /admin")) {
            fprintf(stderr, "FAIL: injected request line forwarded to upstream\n");
            failures++;
        }
        printf("case3 embedded CRLF: injected line not forwarded (n=%zu)\n", n);
    }

    if (failures) {
        fprintf(stderr, "\n%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("\nALL vite_build_headers TESTS PASSED\n");
    return 0;
}
