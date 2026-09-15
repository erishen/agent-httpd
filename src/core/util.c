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
 * to these values when the env is unset. */
int g_cgi_body_tmp_threshold = CGI_BODY_TMP_THRESHOLD_DEFAULT;
int g_cgi_timeout_seconds = CGI_TIMEOUT_SECONDS_DEFAULT;
int g_request_timeout_seconds = REQUEST_TIMEOUT_SECONDS_DEFAULT;

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
