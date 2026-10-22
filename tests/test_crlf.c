/* Unit test for the strict-CRLF request policy (http_parse.c, P12 audit
 * follow-up). RFC 9110/7230 require CRLF line endings; the historical
 * parser tolerated bare-LF and lone-CR terminators. The strict default now
 * rejects them (malformed request); AGENTHTTPD_LENIENT_CRLF=1 /
 * http_crlf_set_lenient(1) restores the old tolerance. Cases:
 *   1. well-formed CRLF request parses under strict;
 *   2. bare-LF line terminator rejected under strict;
 *   3. lone-CR terminator rejected under strict;
 *   4. bare-LF blank line rejected under strict;
 *   5. lenient mode accepts the bare-LF and lone-CR shapes again;
 *   6. strict is restored after lenient (the setter is not sticky).
 * Compiled together with src/http/http_parse.c; no live server required. */
#include <stdio.h>
#include <string.h>
#include "httpd.h"
#include "internal.h"

/* stub: bounded copy mirroring src/core/util.c — the unit test compiles the
 * parser TU alone, and parse_header_line goes through set_str for every
 * field it fills. */
void set_str(char *dst, size_t dst_size, const char *src) {
    if (!dst_size) return;
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* stub: trim in place, same contract as src/core/util.c (parse_header_line
 * trims header values before they hit set_str). */
void trim_whitespace(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int failures = 0;

static void expect(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static const char *CRLF_OK = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
static const char *LF_LINE = "GET /a HTTP/1.1\nHost: x\r\n\r\n";
static const char *CR_LINE = "GET /a HTTP/1.1\rHost: x\r\n\r\n";
static const char *LF_EMPTY = "GET /a HTTP/1.1\r\nHost: x\r\n\n";
static const char *CRLF_EMPTY = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
static const char *HEADER_LF = "GET /a HTTP/1.1\r\nHost: x\n\r\n";

int main(void) {
    HttpRequest req;

    /* 1-4: strict default (0). */
    http_crlf_set_lenient(0);
    memset(&req, 0, sizeof req);
    expect(parse_request(CRLF_OK, &req) == 0, "strict: CRLF request accepted");
    memset(&req, 0, sizeof req);
    expect(parse_request(LF_LINE, &req) == -1, "strict: bare-LF request line rejected");
    memset(&req, 0, sizeof req);
    expect(parse_request(CR_LINE, &req) == -1, "strict: lone-CR request line rejected");
    memset(&req, 0, sizeof req);
    expect(parse_request(LF_EMPTY, &req) == -1, "strict: bare-LF blank line rejected");
    memset(&req, 0, sizeof req);
    expect(parse_request(HEADER_LF, &req) == -1, "strict: bare-LF header terminator rejected");
    memset(&req, 0, sizeof req);
    expect(parse_request(CRLF_EMPTY, &req) == 0, "strict: CRLF blank line accepted");

    /* 5: lenient mode restores tolerance. */
    http_crlf_set_lenient(1);
    memset(&req, 0, sizeof req);
    expect(parse_request(LF_LINE, &req) == 0, "lenient: bare-LF accepted");
    memset(&req, 0, sizeof req);
    expect(parse_request(CR_LINE, &req) == 0, "lenient: lone-CR accepted");
    memset(&req, 0, sizeof req);
    expect(parse_request(LF_EMPTY, &req) == 0, "lenient: bare-LF blank line accepted");

    /* 6: setter is per-call, not sticky. */
    http_crlf_set_lenient(0);
    memset(&req, 0, sizeof req);
    expect(parse_request(LF_LINE, &req) == -1, "strict restored after lenient");

    if (failures == 0)
        printf("ok test_crlf: strict CRLF policy + lenient escape hatch (6 shapes)\n");
    return failures ? 1 : 0;
}
