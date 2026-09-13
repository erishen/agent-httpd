/* Response serialization + error pages: turn an HttpResponse into a wire
 * buffer (build_response), render a status error page or fall back to the
 * inline template (set_error_response), and the one-shot client-cache purge
 * switch (purge_client_cache). The keep-alive connection loop that calls
 * build_response lives in src/http.c; request parsing in src/http_parse.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#include "internal.h"
#include "httpd.h"

void set_error_response(HttpResponse *response, int status_code, const char *status_text) {
    char error_page[MAX_PATH_SIZE];
    snprintf(error_page, sizeof(error_page), "%s/error/%d.html", WEB_ROOT, status_code);

    response->status_code = status_code;
    strncpy(response->status_text, status_text, sizeof(response->status_text) - 1);
    strcpy(response->content_type, "text/html");

    read_file_into_response(error_page, response);
    if (response->body) {
        return;
    }

    char page[MAX_REQUEST_SIZE];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html>\n<html>\n<head><title>%d %s</title></head>\n"
             "<body>\n<h1>%d %s</h1>\n<p>%s</p>\n<hr>\n<p>%s</p>\n</body>\n</html>\n",
             status_code, status_text, status_code, status_text, status_text, SERVER_VERSION);
    response->body = strdup(page);
    response->body_length = strlen(page);
}

/* One-shot eviction for caches poisoned before the no-store policy existed.
 * A response header cannot retract a copy a browser stored earlier, but
 * Clear-Site-Data makes it drop this origin's HTTP cache, so the next fetch
 * is guaranteed fresh rather than merely revalidated. Enabled per process
 * from PURGE_CLIENT_CACHE (any value but "0"); read once so the response
 * path never calls getenv() per request. Meant to be switched on only until
 * every client has visited once - while on, repeat visitors re-evict. */
static int purge_client_cache(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("PURGE_CLIENT_CACHE");
        cached = (v && *v && strcmp(v, "0") != 0);
    }
    return cached;
}

/* RFC 7230 section 6.3: the last response on a connection carries
 * "Connection: close"; HTTP/1.1 defaults to persistent otherwise. */
int build_response(const HttpResponse *response, int head_only, int keep_alive, char *raw_response, int *response_len) {
    char *p = raw_response;
    char *end = raw_response + MAX_RESPONSE_SIZE - 1;

    p += snprintf(p, end - p + 1, "HTTP/1.1 %d %s\r\n", response->status_code, response->status_text);
    /* RFC 9110 6.6.1: Date is required on every response. IMF-fixdate,
     * always GMT; the C locale keeps %a/%b in English. */
    {
        time_t now = time(NULL);
        struct tm tm_gmt;
        char date_str[40];
        gmtime_r(&now, &tm_gmt);
        strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", &tm_gmt);
        p += snprintf(p, end - p + 1, "Date: %s\r\n", date_str);
    }
    /* RFC 9110 8.6 + 15.4.5: a 304 describes no body — omit the headers
     * a 200 would carry for the same resource (Content-Type belongs to the
     * representation; a zero Content-Length only invites proxy ambiguity).
     * HEAD keeps Content-Length: it stands in for the GET that would have
     * run (9.3.2). */
    int omit_entity_headers = (response->status_code == 304);
    if (!omit_entity_headers) {
        p += snprintf(p, end - p + 1, "Content-Type: %s\r\n",
                      response->content_type[0] ? response->content_type : "text/html");
        p += snprintf(p, end - p + 1, "Content-Length: %d\r\n", response->body_length);
    }
    if (response->content_encoding[0]) {
        p += snprintf(p, end - p + 1, "Content-Encoding: %s\r\n", response->content_encoding);
    }
    if (response->cache_control[0]) {
        /* Outside the 304 entity-header skip: a revalidation answer has to
         * repeat the cache policy (RFC 9111 4.3.4), or a cache that stored
         * the response from the 304 alone loses it. */
        p += snprintf(p, end - p + 1, "Cache-Control: %s\r\n", response->cache_control);
    }
    /* Clear-Site-Data rides only on documents: evicting the origin cache on
     * every static asset would drop the bundle once per page view, which is
     * the opposite of what the revalidation policy above is for. */
    if (purge_client_cache() && strncmp(response->content_type, "text/html", 9) == 0) {
        p += snprintf(p, end - p + 1, "Clear-Site-Data: \"cache\"\r\n");
    }
    if (response->location[0]) {
        p += snprintf(p, end - p + 1, "Location: %s\r\n", response->location);
    }
    if (response->status_code == 401 && response->auth_realm) {
        p += snprintf(p, end - p + 1, "WWW-Authenticate: Basic realm=\"%s\", charset=\"UTF-8\"\r\n",
                      response->auth_realm);
    }
    if (response->status_code == 429 && response->retry_after > 0) {
        p += snprintf(p, end - p + 1, "Retry-After: %d\r\n", response->retry_after);
    }
    if (response->allow[0]) {
        p += snprintf(p, end - p + 1, "Allow: %s\r\n", response->allow);
    }
    if (response->etag[0]) {
        /* Only static 200/304 responses set an ETag; those are also the
         * paths where gzip negotiation happens, so advertise Vary here. */
        p += snprintf(p, end - p + 1, "ETag: %s\r\n", response->etag);
        p += snprintf(p, end - p + 1, "Vary: Accept-Encoding\r\n");
    }
    if (response->status_code == 206 ||
        (response->etag[0] && response->status_code != 304)) {
        p += snprintf(p, end - p + 1, "Accept-Ranges: bytes\r\n");
    }
    if (response->content_range[0] &&
        (response->status_code == 206 || response->status_code == 416)) {
        p += snprintf(p, end - p + 1, "Content-Range: %s\r\n", response->content_range);
    }
    if (response->last_modified[0]) {
        p += snprintf(p, end - p + 1, "Last-Modified: %s\r\n", response->last_modified);
    }
    p += snprintf(p, end - p + 1, "Server: AgentHTTPD\r\n");
    /* Security headers: no version fingerprint; refuse framing, MIME
     * sniffing and referrer leakage on every response. */
    p += snprintf(p, end - p + 1, "X-Content-Type-Options: nosniff\r\n");
    p += snprintf(p, end - p + 1, "X-Frame-Options: DENY\r\n");
    p += snprintf(p, end - p + 1, "Referrer-Policy: no-referrer\r\n");
    /* Content-Security-Policy: lock to same-origin, block framing, plugins
     * and inline base URIs. A Vite-built single-page app loads its own
     * bundle.js and issues same-origin fetches, so 'self' is sufficient;
     * if the app ever needs inline scripts/eval, relax script-src here.
     * frame-ancestors 'none' is the modern replacement for X-Frame-Options. */
    p += snprintf(p, end - p + 1,
        "Content-Security-Policy: default-src 'self'; "
        "script-src 'self'; style-src 'self' 'unsafe-inline'; "
        "img-src 'self' data:; font-src 'self'; connect-src 'self'; "
        "frame-ancestors 'none'; base-uri 'self'; form-action 'self'; "
        "object-src 'none'\r\n");
    /* Process-isolation + cross-origin resource guards (spectre-class
     * side channels, accidental cross-origin loads). */
    p += snprintf(p, end - p + 1, "Cross-Origin-Opener-Policy: same-origin\r\n");
    p += snprintf(p, end - p + 1, "Cross-Origin-Resource-Policy: same-origin\r\n");
    p += snprintf(p, end - p + 1, "X-Permitted-Cross-Domain-Policies: none\r\n");
    if (keep_alive) {
        p += snprintf(p, end - p + 1, "Connection: keep-alive\r\n");
    } else {
        p += snprintf(p, end - p + 1, "Connection: close\r\n");
    }
    p += snprintf(p, end - p + 1, "\r\n");

    int body_sent = 0;
    if (!head_only && response->body && response->body_length > 0) {
        int to_copy = response->body_length;
        if (p + to_copy > end) {
            to_copy = end - p;
        }
        memcpy(p, response->body, to_copy);
        p += to_copy;
        body_sent = to_copy;
    }

    *response_len = p - raw_response;
    return body_sent;
}
