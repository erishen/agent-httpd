/* Unit test for the framework same-origin gate (request_is_same_origin,
 * src/core/util.c). Called by process_request for every POST/PUT/PATCH/
 * DELETE before dispatch (CSRF): Sec-Fetch-Site (browser's own verdict)
 * wins when present; else the Origin header's host[:port] must match the
 * request Host; else (no browser signal) SameSite=Lax semantics allow
 * (curl, server-to-server, top-level navigation). Cases:
 *   1. Sec-Fetch-Site same-origin / none allowed, same-site / cross-site
 *      rejected — and it outranks a mismatched Origin;
 *   2. Origin host matches (case-insensitive, trailing dot), foreign host
 *      and the sandboxed "null" origin rejected;
 *   3. port equivalence: explicit non-default port must match; Origin's
 *      omitted default port matches an explicit 80/443 in Host;
 *   4. no browser signal (empty both) -> allowed;
 *   5. Origin without scheme tolerated as a bare host.
 * Compiled together with src/core/util.c; no live server required. */
#include <stdio.h>
#include <string.h>
#include "internal.h"

/* util.c also defines host_is_allowed, which references this global that
 * normally lives in framework.c; provide the storage for the link. */
char g_allowed_hosts[2048];

static int failures = 0;

static void expect(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static int verdict(const char *sfs, const char *origin, const char *host) {
    HttpRequest req;
    memset(&req, 0, sizeof req);
    if (sfs) strncpy(req.sec_fetch_site, sfs, sizeof(req.sec_fetch_site) - 1);
    if (origin) strncpy(req.origin, origin, sizeof(req.origin) - 1);
    if (host) strncpy(req.host, host, sizeof(req.host) - 1);
    return request_is_same_origin(&req);
}

int main(void) {
    /* 1: Sec-Fetch-Site is the browser's own verdict. */
    expect(verdict("same-origin", "https://evil.com", "example.com") == 1,
           "sfs same-origin outranks mismatched origin");
    expect(verdict("none", "https://evil.com", "example.com") == 1,
           "sfs none (user-initiated nav) allowed");
    expect(verdict("cross-site", "", "example.com") == 0,
           "sfs cross-site rejected");
    expect(verdict("same-site", "", "example.com") == 0,
           "sfs same-site rejected (subdomain is not same-origin)");

    /* 2: Origin vs Host. */
    expect(verdict("", "https://example.com", "example.com") == 1,
           "matching origin allowed");
    expect(verdict("", "https://EXAMPLE.com", "example.com") == 1,
           "origin host case-insensitive");
    expect(verdict("", "https://example.com.", "example.com") == 1,
           "origin trailing FQDN dot tolerated");
    expect(verdict("", "https://evil.com", "example.com") == 0,
           "foreign origin rejected");
    expect(verdict("", "null", "example.com") == 0,
           "sandboxed 'null' origin rejected");
    expect(verdict("", "https://example.com", "") == 0,
           "origin present but no Host rejected");

    /* 3: port equivalence. */
    expect(verdict("", "https://example.com:8443", "example.com:8443") == 1,
           "non-default origin port matches explicit host port");
    expect(verdict("", "https://example.com:8443", "example.com") == 0,
           "non-default origin port vs bare host rejected");
    expect(verdict("", "https://example.com", "example.com:443") == 1,
           "origin default https port matches explicit 443");
    expect(verdict("", "http://example.com", "example.com:80") == 1,
           "origin default http port matches explicit 80");
    expect(verdict("", "https://example.com", "example.com:8080") == 0,
           "origin default port vs non-default host port rejected");
    expect(verdict("", "https://example.com:8443", "example.com:9090") == 0,
           "origin port vs different host port rejected");

    /* 4: no browser signal -> SameSite=Lax fallback allows. */
    expect(verdict("", "", "example.com") == 1,
           "no headers allowed (curl / server-to-server)");
    expect(verdict("", "", "") == 1,
           "no headers and no Host allowed");

    /* 5: bare-host Origin (no scheme) tolerated. */
    expect(verdict("", "example.com", "example.com") == 1,
           "scheme-less origin treated as bare host");

    if (failures == 0)
        printf("ok test_same_origin: same-origin gate (17 shapes)\n");
    return failures ? 1 : 0;
}
