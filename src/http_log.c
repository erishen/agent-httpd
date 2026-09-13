/* Access logging: emit one Combined-Log-Format line per response with the
 * request's control characters sanitized (log_field) and the query string
 * stripped from the path (log_request). The global g_log_fp is defined in
 * src/http.c; the connection loop that calls log_request lives in src/http.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#include "internal.h"
#include "httpd.h"
#include "metrics.h"

/* Replace control characters (including CR/LF) so a crafted Referer or
 * User-Agent cannot forge a new log line or break Combined Log Format
 * parsing. */
static void log_field(const char *src, char *dst, size_t n) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    dst[i] = '\0';
}

void log_request(const char *client_ip, const HttpRequest *request, int status_code, int bytes) {
    /* Metrics: classify by status family. Runs on every path (fast loop,
     * workers, fork mode) since they all log here. */
    if (status_code >= 100 && status_code < 600) {
        METRICS_INC_AT(responses_total, status_code / 100 - 1);
    }
    if (!g_log_fp) return;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%d/%b/%Y:%H:%M:%S %z", &tm_now);
    char ref_buf[sizeof(request->referer)];
    char ua_buf[sizeof(request->user_agent)];
    log_field(request->referer, ref_buf, sizeof(ref_buf));
    log_field(request->user_agent, ua_buf, sizeof(ua_buf));
    /* Log only the path, never the query string: URLs commonly carry tokens,
     * session ids or PII in ?... that must not be persisted to the access
     * log (which is otherwise 0600). */
    char path_buf[MAX_PATH_SIZE];
    const char *q = strchr(request->path, '?');
    size_t pcut = q ? (size_t)(q - request->path) : strlen(request->path);
    if (pcut >= sizeof path_buf) pcut = sizeof path_buf - 1;
    memcpy(path_buf, request->path, pcut);
    path_buf[pcut] = '\0';
    fprintf(g_log_fp, "%s - - [%s] \"%s %s %s\" %d %d \"%s\" \"%s\"\n",
            client_ip, time_str,
            request->method, path_buf,
            request->protocol[0] ? request->protocol : "-",
            status_code, bytes,
            ref_buf[0] ? ref_buf : "-",
            ua_buf[0] ? ua_buf : "-");
    fflush(g_log_fp);
}
