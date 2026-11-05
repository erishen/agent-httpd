/* Regression for the X-Forwarded-For value offset. "X-Forwarded-For:" is
 * 16 bytes and every other header copies from `line + strlen(prefix)`, but
 * this branch used to copy from `line + 17`. With the conventional space
 * after the colon trim_whitespace() hid the mistake; on the compact form
 * "X-Forwarded-For:1.2.3.4" it dropped the first value byte, so trusted-
 * proxy rate limiting keyed ".2.3.4" instead of the client IP.
 *
 * Compiled together with src/http/http_parse.c; no live server required.
 * set_str / trim_whitespace are stubbed exactly as in test_crlf.c because
 * the parser TU is linked alone. */
#include <stdio.h>
#include <string.h>
#include "httpd.h"
#include "internal.h"

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

static int failures = 0;

static void expect_xff(const char *raw, const char *want, const char *what) {
    HttpRequest req;
    memset(&req, 0, sizeof req);
    if (parse_request(raw, &req) != 0) {
        fprintf(stderr, "FAIL: %s: request rejected\n", what);
        failures++;
        return;
    }
    if (strcmp(req.x_forwarded_for, want) != 0) {
        fprintf(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n",
                what, req.x_forwarded_for, want);
        failures++;
    }
}

int main(void) {
    expect_xff("GET /a HTTP/1.1\r\nHost: x\r\n"
               "X-Forwarded-For: 1.2.3.4\r\n\r\n",
               "1.2.3.4", "spaced header");
    expect_xff("GET /a HTTP/1.1\r\nHost: x\r\n"
               "X-Forwarded-For:1.2.3.4\r\n\r\n",
               "1.2.3.4", "compact header (off-by-one regression)");
    expect_xff("GET /a HTTP/1.1\r\nHost: x\r\n"
               "X-Forwarded-For:  203.0.113.7, 10.0.0.1\r\n\r\n",
               "203.0.113.7, 10.0.0.1", "chain with leading spaces");
    expect_xff("GET /a HTTP/1.1\r\nHost: x\r\n\r\n",
               "", "header absent");

    if (failures == 0)
        printf("ok test_xff: compact + spaced + chained + absent XFF values\n");
    return failures ? 1 : 0;
}
