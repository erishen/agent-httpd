/* Unit test for build_response extra-header handling (http_resp.c).
 * Verifies:
 *   1. the default security block (CSP / X-Frame-Options DENY / nosniff /
 *      Referrer-Policy) is emitted on a plain response;
 *   2. a handler-supplied extra header of the same name suppresses the
 *      framework default instead of doubling it (duplicate X-Frame-Options
 *      is a browser error);
 *   3. name matching is case-insensitive (clients may vary case);
 *   4. the config-level g_csp policy replaces the built-in default CSP,
 *      and a per-response extra CSP line wins over g_csp.
 * Compiled together with src/http/http_resp.c under ASan+UBSan; the two
 * externs the TU references (g_csp, read_file_into_response) are stubbed
 * here, no live server required. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "httpd.h"

char g_csp[256];
/* stubbed: build_response references it for the Clear-Site-Data path */
void read_file_into_response(const char *file_path, HttpResponse *response) {
    (void)file_path;
    response->body = NULL;
    response->body_length = 0;
}

static int failures = 0;

static void expect(int cond, const char *what, const char *raw, int rlen) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n--- response head ---\n%.*s\n----------\n",
                what, rlen > 600 ? 600 : rlen, raw);
        failures++;
    }
}

static int resp(HttpResponse *r, char *buf) {
    int len = 0;
    build_response(r, 0, 0, buf, &len);
    buf[len] = '\0';
    return len;
}

int main(void) {
    char buf[MAX_RESPONSE_SIZE + 1];

    /* 1) plain response: full default security block */
    {
        HttpResponse r;
        memset(&r, 0, sizeof(r));
        r.status_code = 200;
        strcpy(r.status_text, "OK");
        strcpy(r.content_type, "text/html");
        g_csp[0] = '\0';
        int n = resp(&r, buf);
        expect(strstr(buf, "Content-Security-Policy: default-src 'self';") != NULL,
               "default CSP emitted", buf, n);
        expect(strstr(buf, "X-Frame-Options: DENY\r\n") != NULL,
               "default X-Frame-Options DENY", buf, n);
        expect(strstr(buf, "X-Content-Type-Options: nosniff\r\n") != NULL,
               "default nosniff", buf, n);
        expect(strstr(buf, "Referrer-Policy: no-referrer\r\n") != NULL,
               "default Referrer-Policy", buf, n);
        printf("case1 default security block: ok\n");
    }

    /* 2) handler sets X-Frame-Options: default DENY suppressed, rest intact */
    {
        HttpResponse r;
        memset(&r, 0, sizeof(r));
        r.status_code = 200;
        strcpy(r.status_text, "OK");
        strcpy(r.content_type, "text/html");
        g_csp[0] = '\0';
        snprintf(r.extra_headers, sizeof(r.extra_headers),
                 "X-Frame-Options: SAMEORIGIN\r\nX-Custom: 1\r\n");
        int n = resp(&r, buf);
        expect(strstr(buf, "X-Frame-Options: SAMEORIGIN\r\n") != NULL,
               "custom X-Frame-Options emitted", buf, n);
        expect(strstr(buf, "X-Frame-Options: DENY") == NULL,
               "default DENY suppressed", buf, n);
        expect(strstr(buf, "X-Custom: 1\r\n") != NULL, "extra line verbatim", buf, n);
        expect(strstr(buf, "X-Content-Type-Options: nosniff\r\n") != NULL,
               "unrelated defaults kept", buf, n);
        printf("case2 X-Frame-Options override: ok\n");
    }

    /* 3) case-insensitive suppression */
    {
        HttpResponse r;
        memset(&r, 0, sizeof(r));
        r.status_code = 200;
        strcpy(r.status_text, "OK");
        strcpy(r.content_type, "text/html");
        g_csp[0] = '\0';
        snprintf(r.extra_headers, sizeof(r.extra_headers),
                 "x-frame-options: SAMEORIGIN\r\n");
        int n = resp(&r, buf);
        expect(strstr(buf, "X-Frame-Options: DENY") == NULL,
               "lower-case extra suppresses default", buf, n);
        printf("case3 case-insensitive name match: ok\n");
    }

    /* 4) per-response CSP beats config g_csp; g_csp replaces the default */
    {
        HttpResponse r;
        memset(&r, 0, sizeof(r));
        r.status_code = 200;
        strcpy(r.status_text, "OK");
        strcpy(r.content_type, "text/html");
        snprintf(g_csp, sizeof(g_csp), "default-src 'self'; frame-ancestors http://10.0.0.1:16082");
        int n = resp(&r, buf);
        expect(strstr(buf, "Content-Security-Policy: default-src 'self'; frame-ancestors http://10.0.0.1:16082\r\n") != NULL,
               "config CSP emitted verbatim", buf, n);
        expect(strstr(buf, "script-src 'self'") == NULL,
               "built-in default CSP gone under config CSP", buf, n);

        HttpResponse r2;
        memset(&r2, 0, sizeof(r2));
        r2.status_code = 200;
        strcpy(r2.status_text, "OK");
        strcpy(r2.content_type, "text/html");
        snprintf(r2.extra_headers, sizeof(r2.extra_headers),
                 "Content-Security-Policy: default-src *\r\n");
        int n2 = resp(&r2, buf);
        expect(strstr(buf, "Content-Security-Policy: default-src *\r\n") != NULL,
               "response CSP wins", buf, n2);
        expect(strstr(buf, "frame-ancestors http://10.0.0.1:16082") == NULL,
               "config CSP suppressed by response CSP", buf, n2);
        printf("case4 CSP selection order: ok\n");
    }

    /* 5) unknown-name extra header suppresses nothing */
    {
        HttpResponse r;
        memset(&r, 0, sizeof(r));
        r.status_code = 200;
        strcpy(r.status_text, "OK");
        strcpy(r.content_type, "text/html");
        g_csp[0] = '\0';
        snprintf(r.extra_headers, sizeof(r.extra_headers),
                 "X-Custom-Thing: yes\r\n");
        int n = resp(&r, buf);
        expect(strstr(buf, "X-Frame-Options: DENY\r\n") != NULL,
               "unrelated extra keeps all defaults", buf, n);
        printf("case5 unrelated extra header: ok\n");
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all resp-header cases passed\n");
    return 0;
}
