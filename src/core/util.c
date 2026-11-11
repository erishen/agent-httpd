/* Small shared helpers: environment-tuned guard rails and the string,
 * URL and HTML utilities used by several modules. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "internal.h"

/* Format a peer sockaddr (IPv4 or IPv6) into a printable string, writing at
 * most n bytes into buf. Used for CGI REMOTE_ADDR, the access-log client_ip,
 * and the per-connection ip_str in the master event loop. Returns buf. */
const char *sockaddr_to_str(const struct sockaddr *sa, char *buf, size_t n) {
    if (!buf || n == 0) return buf;
    /* "-" (the Apache CLF placeholder) for an absent/unknown peer keeps the
     * access log and CGI REMOTE_ADDR well-formed. */
    if (!sa) { snprintf(buf, n, "-"); return buf; }
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &sin->sin_addr, buf, n);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &sin6->sin6_addr, buf, n);
    } else {
        snprintf(buf, n, "-");
    }
    return buf;
}

/* Guard rails, overridable via environment for tests (see httpd.h).
 * main() applies -T and the env overrides; the getters below fall back
 * to these values when the env is unset. The request-body spool cap
 * (g_body_spool_bytes) lives in http_parse.c so that file's standalone
 * unit test stays self-contained. */
int g_cgi_body_tmp_threshold = CGI_BODY_TMP_THRESHOLD_DEFAULT;
int g_cgi_timeout_seconds = CGI_TIMEOUT_SECONDS_DEFAULT;
int g_request_timeout_seconds = REQUEST_TIMEOUT_SECONDS_DEFAULT;
char g_body_tmp_dir[128] = BODY_TMP_DIR;

int env_int(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *end = NULL;
    long r = strtol(v, &end, 10);
    if (end == v || r < 0 || r > 86400) return dflt;
    return (int)r;
}

int get_request_timeout(void) {
    return env_int("REQUEST_TIMEOUT_SECONDS", g_request_timeout_seconds);
}

int get_cgi_timeout(void) {
    return env_int("CGI_TIMEOUT_SECONDS", g_cgi_timeout_seconds);
}

int get_cgi_body_tmp_threshold(void) {
    return env_int("CGI_BODY_TMP_THRESHOLD", g_cgi_body_tmp_threshold);
}

/* Forward scan clipped at max: advance whole UTF-8 sequences while the
 * next one fits within max; stop otherwise. Returns the byte length of the
 * last completed code point. The caller's source may itself already be a
 * byte-truncated string, so validity is judged by the sequence contents,
 * never by the string length. */
size_t utf8_valid_prefix(const char *s, size_t max) {
    if (!s) return 0;
    size_t pos = 0, end = 0;
    while (pos < max && s[pos]) {
        unsigned char b = (unsigned char)s[pos];
        size_t width;
        if (b >= 0xF0) width = 4;
        else if (b >= 0xE0) width = 3;
        else if (b >= 0xC2) width = 2;
        else width = 1;
        if (pos + width > max) break;
        pos += width;
        end = pos;
    }
    return end;
}

/* set_str bounded to a UTF-8-safe prefix: never leaves a partial multi-byte
 * character in dst (plain set_str + a fixed SKILL_DESC_MAX/TOOL_DESC_MAX cap
 * truncated mid-char and persisted invalid bytes into catalogs/JSON/prompts). */
void set_str_utf8(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) return;
    size_t n = src ? utf8_valid_prefix(src, dst_size - 1) : 0;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void trim_whitespace(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

const char *get_content_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".webp") == 0) return "image/webp";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".txt") == 0 || strcmp(ext, ".text") == 0) return "text/plain";
    if (strcmp(ext, ".xml") == 0) return "application/xml";
    if (strcmp(ext, ".pdf") == 0) return "application/pdf";
    if (strcmp(ext, ".cgi") == 0) return "application/x-executable";
    if (strcmp(ext, ".webmanifest") == 0) return "application/manifest+json";
    if (strcmp(ext, ".wasm") == 0) return "application/wasm";
    if (strcmp(ext, ".woff") == 0) return "font/woff";
    if (strcmp(ext, ".woff2") == 0) return "font/woff2";
    if (strcmp(ext, ".mjs") == 0) return "application/javascript";

    return "application/octet-stream";
}

void url_decode(char *dst, const char *src) {
    char *p = dst;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int hex = 0;
            sscanf(src + 1, "%2x", &hex);
            *p++ = (char)hex;
            src += 3;
        } else if (*src == '+') {
            *p++ = ' ';
            src++;
        } else {
            *p++ = *src++;
        }
    }
    *p = '\0';
}

void html_escape(const char *src, char *dst, size_t dst_size) {
    char *p = dst;
    const char *end = dst + dst_size - 1;
    while (*src && p < end) {
        switch (*src) {
            case '&': p += snprintf(p, end - p + 1, "&amp;"); break;
            case '<': p += snprintf(p, end - p + 1, "&lt;"); break;
            case '>': p += snprintf(p, end - p + 1, "&gt;"); break;
            case '"': p += snprintf(p, end - p + 1, "&quot;"); break;
            default: *p++ = *src;
        }
        src++;
    }
    *p = '\0';
}

/* Bounded string set: always NUL-terminates (strncpy does not when src
 * fills dst exactly); silences -Wstringop-truncation on GCC 12+ -O2. */
void set_str(char *dst, size_t dst_size, const char *src) {
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* True when any '/'-separated component of the decoded path starts with
 * '.' (".hidden", ".."; empty components are ignored). Static/CGI serving
 * refuses such paths so a stray dotfile (editor state, .env, VCS metadata)
 * dropped into the docroot is not exposed over HTTP, and directory
 * listings hide the same entries. */
int path_has_dot_component(const char *path) {
    const char *p = path;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        if (p > seg && seg[0] == '.') return 1;
        if (*p) p++;
    }
    return 0;
}

/* Split a Host: header value (or an allowlist entry) into its hostname part
 * and, when present, the port suffix. The hostname is lowercased, a single
 * trailing dot (FQDN absolute form) is stripped, and IPv6 brackets are
 * removed ("[::1]:8080" -> "::1" + port "8080"). *port_out is set to the
 * port text or NULL. Returns 0 on success, -1 on a malformed value. */
static int split_host(const char *s, char *host_out, size_t outsz,
                      const char **port_out) {
    const char *h = s;
    const char *hend = NULL;
    *port_out = NULL;
    if (s[0] == '[') {
        const char *br = strchr(s, ']');
        if (!br) return -1;              /* malformed IPv6 literal */
        h = s + 1;
        hend = br;
        if (br[1] == ':') *port_out = br + 2;
    } else {
        const char *colon = strchr(s, ':');
        if (colon && strchr(colon + 1, ':') == NULL) {
            /* exactly one ':' -> host:port; a second ':' means a bracketless
             * IPv6 literal (e.g. "::1"), which is the whole hostname. */
            hend = colon;
            *port_out = colon + 1;
        } else {
            hend = s + strlen(s);
        }
    }
    size_t n = (size_t)(hend - h);
    if (n >= outsz) n = outsz - 1;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        host_out[o++] = c;
    }
    while (o > 0 && host_out[o - 1] == '.') o--; /* trailing FQDN dot */
    host_out[o] = '\0';
    return 0;
}

/* Host allowlist predicate (ALLOWED_HOSTS, config `allowed_hosts`). Empty
 * allowlist accepts anything (the caller short-circuits on it anyway);
 * otherwise the request's Host: header must match an entry. Hostnames
 * compare case-insensitively after stripping the port and a trailing dot;
 * an entry that itself carries ":port" matches only that exact host:port,
 * while a bare-hostname entry accepts any port. Malformed entries and
 * malformed Host values are rejected — they never widen the allowlist. */
int host_is_allowed(const char *host) {
    if (!g_allowed_hosts[0]) return 1;   /* no policy: accept any */
    if (!host || !host[0]) return 0;     /* policy set, no Host: reject */
    char hname[256];
    const char *hport;
    if (split_host(host, hname, sizeof hname, &hport) != 0) return 0;
    const char *p = g_allowed_hosts;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t elen = (size_t)(end - start);
        char ebuf[256];
        if (elen >= sizeof ebuf) elen = sizeof ebuf - 1;
        memcpy(ebuf, start, elen);
        ebuf[elen] = '\0';
        char ename[256];
        const char *eport;
        if (split_host(ebuf, ename, sizeof ename, &eport) != 0) continue;
        if (strcmp(hname, ename) == 0) {
            if (eport == NULL) return 1;         /* bare host: any port ok */
            if (hport != NULL && strcmp(hport, eport) == 0) return 1;
        }
    }
    return 0;
}

/* Same-origin verdict for the state-changing method gate (CSRF). Chain:
 *   1. Sec-Fetch-Site (the browser's own cross-site verdict, hardest to
 *      spoof into a false allow): same-origin / none -> ok; same-site /
 *      cross-site -> reject.
 *   2. Origin header: its host[:port] must match the request Host (which,
 *      when ALLOWED_HOSTS is set, is itself allowlisted). Scheme-default
 *      ports (an omitted port in Origin) are treated as equivalent to an
 *      explicit 80/443 in Host; a non-default port in Origin with no port
 *      in Host is rejected.
 *   3. Neither signal (curl, server-to-server calls, old browsers, top-level
 *      navigations): SameSite=Lax semantics -> allow.
 * A framework default on purpose: process_request calls this for every
 * POST/PUT/PATCH/DELETE before dispatch, so a forgotten per-handler guard
 * cannot reopen the hole. */
int request_is_same_origin(const HttpRequest *req) {
    /* 配了 CORS 白名单时，白名单 Origin 等价于原项目 CORSMiddleware：
     * 允许跨域带凭据的写请求（web/admin/firefly 前端直连 API 的场景）。
     * 精确匹配，未配置或来源不在名单内仍走下面的同源判定。 */
    if (req->origin[0] && cors_origin_allowed(req->origin)) return 1;
    if (req->sec_fetch_site[0]) {
        if (strcmp(req->sec_fetch_site, "same-origin") == 0 ||
            strcmp(req->sec_fetch_site, "none") == 0)
            return 1;
        return 0; /* same-site / cross-site: not same-origin */
    }
    if (req->origin[0]) {
        const char *oh = strstr(req->origin, "://");
        oh = oh ? oh + 3 : req->origin; /* "null" (sandboxed iframe) lands here */
        char ohost[256];
        char hhost[256];
        const char *oport = NULL;
        const char *hport = NULL;
        if (split_host(oh, ohost, sizeof ohost, &oport) != 0) return 0;
        if (split_host(req->host, hhost, sizeof hhost, &hport) != 0) return 0;
        if (strcmp(ohost, hhost) != 0) return 0;
        if (oport) {
            if (!hport || strcmp(oport, hport) != 0) return 0;
        } else {
            /* Origin used its scheme-default port; Host may name it
             * explicitly (80 for http, 443 for https). Anything else
             * mismatch. */
            if (hport && strcmp(hport, "80") != 0 && strcmp(hport, "443") != 0)
                return 0;
        }
        return 1;
    }
    return 1; /* no browser signal: SameSite=Lax allows */
}
