/* Unit test for the Host allowlist (host_is_allowed, src/core/util.c;
 * ALLOWED_HOSTS env / server{ allowed_hosts = ... }). When the allowlist is
 * non-empty every request whose Host: header matches no entry is answered
 * 421 Misdirected Request (http.c), which also closes Host-header poisoning.
 * Cases:
 *   1. empty allowlist accepts any host (the historical default);
 *   2. bare-hostname entry: exact case-insensitive match, any port ok,
 *      trailing FQDN dot tolerated, subdomains and look-alikes rejected;
 *   3. ported entry: matches only the exact host:port;
 *   4. CSV with spaces / trailing commas / empty entries;
 *   5. IPv6 literals with and without brackets and ports.
 * Compiled together with src/core/util.c; no live server required. */
#include <stdio.h>
#include <string.h>
#include "internal.h"

/* The unit test links util.c alone; g_allowed_hosts normally lives in
 * framework.c (set by agenthttpd_run from the config field or ALLOWED_HOSTS
 * env). Provide the storage here and set it directly per case. */
char g_allowed_hosts[2048];

static int failures = 0;

static void expect(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void set_list(const char *csv) {
    memset(g_allowed_hosts, 0, sizeof(g_allowed_hosts));
    if (csv) {
        size_t n = strlen(csv);
        if (n >= sizeof(g_allowed_hosts)) n = sizeof(g_allowed_hosts) - 1;
        memcpy(g_allowed_hosts, csv, n);
        g_allowed_hosts[n] = '\0';
    }
}

int main(void) {
    /* 1: no policy -> accept anything (host_is_allowed returns 1 even for a
     * missing Host; http.c short-circuits on the empty string anyway). */
    set_list("");
    expect(host_is_allowed("example.com") == 1, "empty allowlist accepts any");
    expect(host_is_allowed("") == 1, "empty allowlist accepts missing Host");

    /* 2: bare-hostname entry. */
    set_list("example.com");
    expect(host_is_allowed("example.com") == 1, "exact host ok");
    expect(host_is_allowed("EXAMPLE.COM") == 1, "case-insensitive match");
    expect(host_is_allowed("Example.Com.") == 1, "trailing FQDN dot ok");
    expect(host_is_allowed("example.com:8080") == 1, "bare entry accepts any port");
    expect(host_is_allowed("sub.example.com") == 0, "subdomain is not allowed");
    expect(host_is_allowed("example.com.evil.com") == 0, "suffix look-alike rejected");
    expect(host_is_allowed("notexample.com") == 0, "prefix look-alike rejected");
    expect(host_is_allowed("evil.com") == 0, "foreign host rejected");
    expect(host_is_allowed("") == 0, "missing Host rejected under policy");

    /* 3: ported entry is exact host:port only. */
    set_list("example.com:8080");
    expect(host_is_allowed("example.com:8080") == 1, "ported entry exact ok");
    expect(host_is_allowed("example.com") == 0, "ported entry refuses bare host");
    expect(host_is_allowed("example.com:9090") == 0, "ported entry refuses other port");

    /* 4: CSV hygiene — spaces, trailing comma, empty fields, mixed entries. */
    set_list("a.com, b.com ");
    expect(host_is_allowed("a.com") == 1, "first csv entry ok");
    expect(host_is_allowed("b.com") == 1, "second csv entry ok (space trimmed)");
    expect(host_is_allowed("c.com") == 0, "csv: foreign host rejected");
    set_list("example.com,");
    expect(host_is_allowed("example.com") == 1, "trailing comma tolerated");
    set_list("example.com,example.com:8443");
    expect(host_is_allowed("example.com:443") == 1, "bare entry still any port");
    expect(host_is_allowed("example.com:8443") == 1, "ported entry matches its port");
    set_list(",");
    expect(host_is_allowed("x.com") == 0, "empty csv accepts nothing");

    /* 5: IPv6 literals. */
    set_list("::1");
    expect(host_is_allowed("[::1]") == 1, "ipv6 bracket form ok");
    expect(host_is_allowed("[::1]:8080") == 1, "ipv6 bare entry any port");
    expect(host_is_allowed("[::2]") == 0, "ipv6 foreign literal rejected");
    set_list("[::1]:8080");
    expect(host_is_allowed("[::1]:8080") == 1, "ipv6 ported entry exact ok");
    expect(host_is_allowed("[::1]:9090") == 0, "ipv6 ported entry other port");

    if (failures == 0)
        printf("ok test_allowed_hosts: Host allowlist predicate (22 shapes)\n");
    return failures ? 1 : 0;
}
