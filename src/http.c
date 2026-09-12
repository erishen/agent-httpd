/* HTTP protocol core: request parsing, response serialization, error
 * pages, access logging, the /react FastCGI relay and the keep-alive
 * connection loop (RFC 7230 section 6.3). main() only accept()s; every
 * accepted connection lands here, for both dispatch models. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <fcntl.h>
#ifdef __linux__
#include <sys/sendfile.h> /* sendfile(2) */
#endif

#include "internal.h"
#include "metrics.h"
#include "llm.h"

volatile sig_atomic_t g_server_running = 1;
volatile sig_atomic_t g_reopen_log = 0;
volatile sig_atomic_t g_resync = 0;
FILE *g_log_fp = NULL;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_server_running = 0;
    } else if (sig == SIGHUP) {
        g_reopen_log = 1;
        g_resync = 1;
    }
}

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
    fprintf(g_log_fp, "%s - - [%s] \"%s %s %s\" %d %d \"%s\" \"%s\"\n",
            client_ip, time_str,
            request->method, request->path,
            request->protocol[0] ? request->protocol : "-",
            status_code, bytes,
            ref_buf[0] ? ref_buf : "-",
            ua_buf[0] ? ua_buf : "-");
    fflush(g_log_fp);
}

/* Headers end at the first blank line ("\r\n\r\n"). Parsing must stop there:
 * beyond it sits the request body, whose bytes (including "\r") belong to the
 * payload and must be passed to the handler untouched. Keep the case-mapping
 * helpers here so the header loop stays compact. */
static const char *find_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) return p;
    }
    return NULL;
}

/* Header values go through the project-wide bounded setter (set_str): the
 * destination is a fixed-size field and is always NUL-terminated, so an
 * over-long header cannot leave a field unterminated. */
static void parse_header_line(const char *line, HttpRequest *request) {
    if (strncasecmp(line, "Host:", 5) == 0) {
        set_str(request->host, sizeof(request->host), line + 5);
        trim_whitespace(request->host);
    } else if (strncasecmp(line, "User-Agent:", 11) == 0) {
        set_str(request->user_agent, sizeof(request->user_agent), line + 11);
        trim_whitespace(request->user_agent);
    } else if (strncasecmp(line, "Referer:", 8) == 0) {
        set_str(request->referer, sizeof(request->referer), line + 8);
        trim_whitespace(request->referer);
    } else if (strncasecmp(line, "Accept:", 7) == 0) {
        set_str(request->accept, sizeof(request->accept), line + 7);
        trim_whitespace(request->accept);
    } else if (strncasecmp(line, "Accept-Encoding:", 16) == 0) {
        set_str(request->accept_encoding, sizeof(request->accept_encoding), line + 16);
        trim_whitespace(request->accept_encoding);
    } else if (strncasecmp(line, "Content-Type:", 13) == 0) {
        set_str(request->content_type, sizeof(request->content_type), line + 13);
        trim_whitespace(request->content_type);
    } else if (strncasecmp(line, "Connection:", 11) == 0) {
        set_str(request->connection, sizeof(request->connection), line + 11);
        trim_whitespace(request->connection);
    } else if (strncasecmp(line, "Authorization:", 14) == 0) {
        set_str(request->authorization, sizeof(request->authorization), line + 14);
        trim_whitespace(request->authorization);
    } else if (strncasecmp(line, "If-None-Match:", 14) == 0) {
        set_str(request->if_none_match, sizeof(request->if_none_match), line + 14);
        trim_whitespace(request->if_none_match);
    } else if (strncasecmp(line, "If-Modified-Since:", 18) == 0) {
        set_str(request->if_modified_since, sizeof(request->if_modified_since), line + 18);
        trim_whitespace(request->if_modified_since);
    } else if (strncasecmp(line, "Expect:", 7) == 0) {
        set_str(request->expect, sizeof(request->expect), line + 7);
        trim_whitespace(request->expect);
    } else if (strncasecmp(line, "Range:", 6) == 0) {
        set_str(request->range, sizeof(request->range), line + 6);
        trim_whitespace(request->range);
    } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
        long cl = strtol(line + 15, NULL, 10);
        request->content_length = (cl > 0 && cl <= MAX_REQUEST_SIZE) ? (int)cl : 0;
    } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
        /* RFC 9110 8.7: only chunked is defined for requests; a TE naming
         * any other (final) coding cannot be framed by this server. If TE
         * and Content-Length coexist, RFC 9110 6.1 says the message is so
         * poisoned it MUST be rejected (smuggling ambiguity) — handled by
         * the caller, which sees both flags. */
        const char *v = line + 18;
        if (find_ci(v, "chunked") && !find_ci(v, "gzip") &&
            !find_ci(v, "deflate") && !find_ci(v, "compress") &&
            !find_ci(v, "identity")) {
            request->chunked = 1;
        } else {
            /* some other transfer coding on a request: mark it so the body
             * read can answer 501 instead of framing garbage */
            request->te_unsupported = 1;
        }
    }
}

/* Parse the start line and headers, bounded by the buffer's length. Returns
 * the byte offset just past the "\r\n\r\n" (start of body) on success, or -1
 * on a malformed request. Never writes into the body region. Callers keep
 * the raw buffer for the body (see read_into_body). */
static long parse_headers(const char *raw_request, long len, HttpRequest *request) {
    long p = 0;
    /* request line */
    while (p < len && raw_request[p] != '\r' && raw_request[p] != '\n') p++;
    if (p == 0 || p >= len) return -1;
    char reqline[MAX_PATH_SIZE + 32];
    size_t rl = (size_t)(p < MAX_PATH_SIZE ? p : MAX_PATH_SIZE);
    memcpy(reqline, raw_request, rl);
    reqline[rl] = '\0';
    if (sscanf(reqline, "%15s %1023s %15s",
               request->method, request->path, request->protocol) != 3) {
        return -1;
    }

    /* header lines */
    for (;;) {
        if (p >= len) return -1;
        /* end of a line: consume its terminator (\r\n or bare \n) */
        if (raw_request[p] == '\r') {
            p++;
            if (p < len && raw_request[p] == '\n') p++;
        } else if (raw_request[p] == '\n') {
            p++;
        } else {
            long start = p;
            while (p < len && raw_request[p] != '\r' && raw_request[p] != '\n') p++;
            if (p == len) return -1;
            char header[MAX_PATH_SIZE];
            /* clamp in long arithmetic: mixing the two operands of a ?: across
             * signed/unsigned promotes the whole expression and trips GCC's
             * -Wsign-compare (fatal under -Werror) */
            long hlen = p - start;
            if (hlen > (long)sizeof header - 1) hlen = (long)sizeof header - 1;
            size_t hl = (size_t)hlen;
            memcpy(header, raw_request + start, hl);
            header[hl] = '\0';
            parse_header_line(header, request);
            continue; /* re-enter at the terminator we just found */
        }
        /* blank line (right after the terminator) closes the header block */
        if (p < len && (raw_request[p] == '\r' || raw_request[p] == '\n')) {
            if (raw_request[p] == '\r' && p + 1 < len && raw_request[p + 1] == '\n') p++;
            p++;
            return p; /* p now points at the body (or the buffer end) */
        }
    }
}

int parse_request(const char *raw_request, HttpRequest *request) {
    memset(request, 0, sizeof(HttpRequest));
    long len = strnlen(raw_request, MAX_REQUEST_SIZE);
    return parse_headers(raw_request, len, request) < 0 ? -1 : 0;
}

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

/* Send the interim 100 Continue response (best effort). Called right
 * before a request body is read from the socket, when the client asked
 * for it via Expect: 100-continue. */
static void send_continue(int client_fd) {
    if (client_fd < 0) return;
    static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
    ssize_t w = send(client_fd, cont, sizeof(cont) - 1, 0);
    (void)w; /* SIGPIPE ignored; a vanished client just won't send the body */
}

int is_fast_request(const HttpRequest *request) {
    /* OPTIONS is answered inline by process_request with an Allow menu (no
     * CGI spawn, no disk beyond a stat on the path) — CORS preflights are
     * frequent enough to deserve the loop. Body-carrying OPTIONS still
     * falls through to the worker pool via the content_length check. */
    if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "HEAD") != 0 &&
        strcmp(request->method, "OPTIONS") != 0) {
        return 0;
    }
    if (request->content_length > 0) return 0; /* body needs the blocking pipeline */
    if (llm_is_chat_route(request->path, request->method)) return 0;
    if (strncmp(request->path, "/react", 6) == 0 &&
        (request->path[6] == '\0' || request->path[6] == '/')) {
        return 0;
    }
    if (is_cgi_request(request->path)) return 0;
    if (g_vite_upstream_port > 0 && is_vite_proxy_route(request)) return 0;
    return 1;
}

int is_vite_proxy_route(const HttpRequest *request) {
    if (g_vite_upstream_port <= 0) return 0;
    if (request->content_length > 0) return 0;
    const char *p = request->path;
    if (strncmp(p, "/@", 2) == 0) return 1; /* /@vite, /@fs, /@id, /@react-refresh */
    if (strcmp(p, "/react/react-ssr.tsx") == 0) return 1;
    if (strncmp(p, "/react", 6) == 0 &&
        (p[6] == '\0' || p[6] == '/') &&
        strcmp(p, "/react/api/chat") != 0) {
        return 1; /* dev SSR pages */
    }
    if (strncmp(p, "/src/", 5) == 0) return 1;
    return 0;
}

int is_websocket_upgrade(const char *raw, size_t len) {
    if (len < 16) return 0;
    if (strncasecmp(raw, "GET", 3) != 0) return 0;
    const char *conn = raw, *end = raw + len;
    /* scan header lines for Connection: ...upgrade... and Upgrade: websocket */
    int has_upgrade = 0, has_connection_upgrade = 0;
    while (conn < end) {
        const char *nl = memchr(conn, '\n', (size_t)(end - conn));
        size_t ll = nl ? (size_t)(nl - conn + 1) : (size_t)(end - conn);
        if (ll > 2 && strncasecmp(conn, "Upgrade:", 8) == 0 &&
            strstr(conn, "websocket")) {
            has_upgrade = 1;
        } else if (ll > 2 && strncasecmp(conn, "Connection:", 11) == 0 &&
                   (strstr(conn, "upgrade") || strstr(conn, "Upgrade"))) {
            has_connection_upgrade = 1;
        }
        if (conn + ll >= end) break;
        conn += ll;
    }
    return has_upgrade && has_connection_upgrade;
}

int process_request(HttpRequest *request, HttpResponse *response, int client_fd) {
    /* Liveness probe for load balancers / uptime checks: fixed tiny 200.
     * Sits behind the rate-limit and auth gates in handle_client, so an
     * authed server requires credentials here too (documented). */
    if (strncmp(request->path, "/health", 7) == 0 &&
        (request->path[7] == '\0' || request->path[7] == '?')) {
        response->status_code = 200;
        strcpy(response->status_text, "OK");
        strcpy(response->content_type, "text/plain");
        response->body = strdup("ok\n");
        response->body_length = response->body ? 3 : 0;
        return 0;
    }

    /* Operations endpoint: Prometheus text format snapshot of the shared
     * counter table (see src/metrics.c). Same gate position as /health —
     * behind rate-limit/auth, ride-alive on the fast path. */
    if (strncmp(request->path, "/metrics", 8) == 0 &&
        (request->path[8] == '\0' || request->path[8] == '?')) {
        char *buf = malloc(8192);
        response->status_code = 200;
        strcpy(response->status_text, "OK");
        strcpy(response->content_type,
                "text/plain; version=0.0.4; charset=utf-8");
        if (buf) {
            response->body_length = metrics_render(buf, 8192);
            response->body = buf;
        } else {
            response->body = NULL;
            response->body_length = 0;
        }
        return 0;
    }

    /* Method dispatch (RFC 9110 9): GET/HEAD read resources, POST/PUT/PATCH
     * carry bodies to CGI, DELETE asks a script to remove something, and
     * OPTIONS is answered by the server itself with an Allow menu. Static
     * files are read-only: writer methods only make sense on CGI paths —
     * a PUT to a static URL gets 405 + Allow (never written to disk). */
    static const char *const READ_METHODS = "GET, HEAD, OPTIONS";
    static const char *const ALL_METHODS =
        "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS";
    if (strcmp(request->method, "GET") == 0 || strcmp(request->method, "HEAD") == 0) {
        /* read methods flow to the dispatch below */
    } else if (strcmp(request->method, "POST") == 0 ||
               strcmp(request->method, "PUT") == 0 ||
               strcmp(request->method, "PATCH") == 0 ||
               strcmp(request->method, "DELETE") == 0) {
        if (!is_cgi_request(request->path)) {
            set_error_response(response, 405, "Method Not Allowed");
            snprintf(response->allow, sizeof response->allow, "%s", READ_METHODS);
            return -1;
        }
    } else if (strcmp(request->method, "OPTIONS") == 0) {
        /* Serve the capability menu inline: scripts stay in charge of the
         * verbs themselves, but the server can truthfully advertise them
         * without executing anything. 200 (not 204): the serializer always
         * emits Content-Length, which RFC 9110 8.6 forbids on 204. */
        response->status_code = 200;
        strcpy(response->status_text, "OK");
        response->body = strdup("ok\n");
        response->body_length = response->body ? 3 : 0;
        snprintf(response->allow, sizeof response->allow, "%s",
                 is_cgi_request(request->path) ? ALL_METHODS : READ_METHODS);
        return 0;
    } else {
        set_error_response(response, 501, "Not Implemented");
        return -1;
    }

    if (is_cgi_request(request->path)) {
        if (request->path[8] == '\0') {
            response->status_code = 301;
            strcpy(response->status_text, "Moved Permanently");
            strcpy(response->location, "/cgi-bin/");
        } else if (request->path[8] == '/' && request->path[9] == '\0') {
            handle_directory(g_cgi_bin_real, "/cgi-bin/", response);
        } else {
            execute_cgi(request, response, client_fd);
        }
    } else {
        /* Static files are read-only; writer methods were 405'd above, so
         * only GET/HEAD reach this branch. */
        handle_static_file(request, response);
    }
    return 0;
}

int create_server_socket(int port) {
    int server_fd;
    struct sockaddr_in server_addr;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return -1;
    }
#ifdef __linux__
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEPORT)");
    }
#endif

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

/* Dev mode: forward one request to the internal Vite server (g_vite_upstream
 * _port) and stream its response back to the client. Used for client module
 * transforms and dev SSR pages — everything the C server cannot render
 * itself. Runs on a blocking pool worker; the header block is replayed
 * verbatim (minus Host/Connection, which we control). */
/* A smuggled request hides inside a header value as an embedded CRLF
 * (e.g. "Foo: bar\r\nGET /x HTTP/1.1\r\n"). We are the framing authority, so
 * reject any header line that carries a CR/LF anywhere except its own
 * CRLF terminator (the last two bytes). */
static void proxy_to_vite(int client_fd, const char *raw, size_t hdr_len,
                          const HttpRequest *req) {
    int up = socket(AF_INET, SOCK_STREAM, 0);
    if (up < 0) {
        close(client_fd);
        return;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    sa.sin_port = htons((uint16_t)g_vite_upstream_port);
    if (connect(up, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        const char *err = "HTTP/1.1 502 Bad Gateway\r\n"
                          "Content-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(client_fd, err, (int)strlen(err), 0);
        close(up);
        close(client_fd);
        return;
    }

    /* request line (verbatim, keeps the query string) */
    const char *nl = memchr(raw, '\n', hdr_len);
    size_t first_len = nl ? (size_t)(nl - raw + 1) : hdr_len;
    /* reject a smuggled request line (embedded CRLF before its terminator) */
    if (line_has_embedded_crlf(raw, first_len)) {
        const char *err = "HTTP/1.1 400 Bad Request\r\n"
                          "Content-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(client_fd, err, (int)strlen(err), 0);
        close(up);
        close(client_fd);
        return;
    }
    if (send(up, raw, first_len, 0) < 0) goto done;

    /* remaining header lines, dropping hop-by-hop headers we control.
     * Beyond Host/Connection, stripping Transfer-Encoding, TE, Keep-Alive,
     * Proxy-*, Upgrade and Trailer is not just RFC 9110 7.6.1 hygiene: a
     * client smuggling a Transfer-Encoding header alongside our
     * Content-Length would desync the upstream's framing (request
     * smuggling). We are the framing authority now: the body is always
     * sent with an exact Content-Length. */
    char buf[16384];
    size_t n = vite_build_headers(buf, sizeof(buf), raw, hdr_len, g_vite_upstream_port);
    if (send(up, buf, (int)n, 0) < 0) goto done;
    if (req->body && req->content_length > 0) {
        size_t left = (size_t)req->content_length;
        const char *bp = req->body;
        while (left > 0) {
            ssize_t w = send(up, bp, left, 0);
            if (w <= 0) goto done;
            bp += w;
            left -= (size_t)w;
        }
    }

    /* stream the upstream response straight back */
    {
        char rbuf[65536];
        for (;;) {
            ssize_t r = recv(up, rbuf, sizeof(rbuf), 0);
            if (r <= 0) break;
            const char *sp = rbuf;
            ssize_t left = r;
            while (left > 0) {
                ssize_t w = send(client_fd, sp, (size_t)left, 0);
                if (w <= 0) { left = 0; break; }
                sp += w;
                left -= w;
            }
        }
    }
done:
    close(up);
    close(client_fd);
}

/* Dev mode: WebSocket upgrade tunnel for Vite HMR. The raw header block
 * (with Sec-WebSocket-Key) is replayed to the internal Vite server, then the
 * two sockets are spliced byte-for-byte until either side closes. */
static void ws_tunnel(int client_fd, const char *raw, size_t hdr_len) {
    int up = socket(AF_INET, SOCK_STREAM, 0);
    if (up < 0) {
        close(client_fd);
        return;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    sa.sin_port = htons((uint16_t)g_vite_upstream_port);
    if (connect(up, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(up);
        close(client_fd);
        return;
    }
    if (send(up, raw, hdr_len, 0) < 0) {
        close(up);
        close(client_fd);
        return;
    }

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        FD_SET(up, &rfds);
        int hi = client_fd > up ? client_fd : up;
        if (select(hi + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char b[65536];
        int from = -1, to = -1;
        if (FD_ISSET(client_fd, &rfds)) { from = client_fd; to = up; }
        else if (FD_ISSET(up, &rfds)) { from = up; to = client_fd; }
        if (from < 0) continue;
        ssize_t n = recv(from, b, sizeof(b), 0);
        if (n <= 0) break;
        const char *sp = b;
        ssize_t left = n;
        while (left > 0) {
            ssize_t w = send(to, sp, (size_t)left, 0);
            if (w <= 0) { left = 0; break; }
            sp += w;
            left -= w;
        }
    }
    close(up);
    close(client_fd);
}

void handle_client(int client_fd, struct sockaddr_in *client_addr) {
    char buffer[MAX_REQUEST_SIZE];
    char response_buffer[MAX_RESPONSE_SIZE];
    const char *client_ip = client_addr ? inet_ntoa(client_addr->sin_addr) : "-";
    int req_to = get_request_timeout();

    /* Every handle_client invocation is by definition off the fast path
     * (a pool worker took the fd, or fork mode forked for it). */
    METRICS_INC_AT(requests_total, 1);

    /* Responses are header block + body in separate sends; TCP_NODELAY keeps
     * Nagle from batching the first tiny write against the delayed-ACK timer
     * (latency spikes of tens of ms on the first byte of every response).
     * Idempotent: harmless if the dispatcher already set it. */
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* This handler does blocking I/O: every send loop below (the in-buffer
     * send(), the sendfile streamer, the CGI/FastCGI relays) treats a short
     * write as fatal. The event loop accepts with O_NONBLOCK because *it*
     * needs that for epoll/kqueue, and hands the very same socket to a pool
     * worker over SCM_RIGHTS - so the flag is still set here. Without this
     * clear, a body larger than the socket send buffer fails with EAGAIN
     * mid-stream, the response is abandoned under a Content-Length that
     * promised all of it, and the client sees a truncated file (from nginx:
     * "upstream prematurely closed connection"; from a browser: a JS bundle
     * that never parses, so the page silently never hydrates). A fast local
     * reader rarely fills the buffer, which is why it looked fine. */
    {
        int fl = fcntl(client_fd, F_GETFL, 0);
        if (fl != -1 && (fl & O_NONBLOCK)) {
            (void)fcntl(client_fd, F_SETFL, fl & ~O_NONBLOCK);
        }
    }

    /* HTTP/1.1 persistent-connection loop (RFC 7230 section 6.3): serve
     * requests back-to-back until the client asks to close, the request or
     * response cap is hit, an error/timeout strikes, or the response length
     * is unknown (streamed relay) - then close. */
    int carry = 0;                  /* bytes of the next pipelined request
                                     * still parked in buffer from a prior
                                     * request (HTTP/1.1 keep-alive pipelining) */
    long prev_consumed = 0;         /* end offset of the last fully-parsed
                                     * request within buffer */
    for (int served = 0;;) {
    HttpRequest request;
    HttpResponse response;
    int total = 0;
    int header_end_len = 0;
    int keep_alive_force_close = 0; /* set by the 401 challenge path */
    int headers_oversized = 0;      /* header block exceeded the buffer */

    if (carry > 0) {
        /* A previous request left bytes of the next pipelined request in
         * buffer. Shift them to the front and keep parsing from there
         * instead of discarding them: the old code zeroed the buffer and
         * re-recv'd, which silently dropped any pipelined request that had
         * already arrived (the client would hang until its timeout). */
        memmove(buffer, buffer + prev_consumed, (size_t)carry);
        total = carry;
        buffer[total] = '\0';
        memset(buffer + total, 0, sizeof(buffer) - (size_t)total);
        carry = 0;
    } else {
        memset(buffer, 0, sizeof(buffer));
    }
    memset(&response, 0, sizeof(response));
    memset(&request, 0, sizeof(request));
    snprintf(request.remote_addr, sizeof(request.remote_addr), "%s", client_ip);

    /* Header read with an overall timeout: a client that dribbles bytes
     * (Slowloris-style) must not pin this forked handler forever. The first
     * request on a connection gets the full request timeout; idle waits for
     * subsequent keep-alive requests use the shorter KEEPALIVE_TIMEOUT. */
    int wait_to = (served == 0) ? req_to : KEEPALIVE_TIMEOUT_SECONDS;
    time_t deadline = wait_to > 0 ? time(NULL) + wait_to : 0;
    /* Parse from what is already buffered before waiting for more: a
     * carried-over pipelined request is complete in `buffer` and no further
     * bytes will ever arrive for it (the client is waiting for our
     * response), so blocking on select()/recv() first parks it until the
     * keep-alive timeout and the client sees a swallowed request. */
    while (1) {
        char *header_end = strstr(buffer, "\r\n\r\n");
        if (header_end) {
            header_end_len = (header_end + 4) - buffer;
            break;
        }
        if (total >= (int)sizeof(buffer) - 1) {
            headers_oversized = 1; /* no \r\n\r\n and no room left */
            break;
        }
        if (deadline && time(NULL) >= deadline) {
            close(client_fd); /* overall request window spent */
            return;
        }
        struct timeval tv = {wait_to > 0 ? wait_to : 300, 0};
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        if (select(client_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            close(client_fd); /* timed out or interrupted waiting for headers */
            return;
        }
        int n = recv(client_fd, buffer + total, sizeof(buffer) - 1 - total, 0);
        if (n <= 0) {
            close(client_fd);
            return;
        }
        total += n;
        buffer[total] = '\0';
    }

    if (headers_oversized) {
        /* RFC 9110 9.5.6: the header block cannot be read as a whole.
         * Close afterwards - the framing state is unrecoverable. */
        set_error_response(&response, 431, "Request Header Fields Too Large");
        keep_alive_force_close = 1;
    } else if (parse_request(buffer, &request) < 0) {
        set_error_response(&response, 400, "Bad Request");
    } else if (deadline && time(NULL) >= deadline) {
        /* headers arrived, but the overall request window is already spent */
        set_error_response(&response, 408, "Request Timeout");
    } else if (g_rate_limit_rps > 0 &&
               !rate_limit_allow(client_addr ? client_addr->sin_addr.s_addr : 0)) {
        /* Checked before auth on purpose: a flood must not reach dispatch or
         * burn crypt() CPU. 429 + Retry-After, then close (queued requests
         * from the same abuser would otherwise smuggle past the counter). */
        set_error_response(&response, 429, "Too Many Requests");
        response.retry_after = 1;
        keep_alive_force_close = 1;
    } else if (!check_basic_auth(request.authorization)) {
        /* 401 challenge. The relay path above answers only when the backend
         * replied; auth runs first, so the backend never sees
         * unauthenticated traffic either. After a 401 the connection is not
         * reusable with certainty (pipelined body state), so close it. */
        set_error_response(&response, 401, "Unauthorized");
        response.auth_realm = g_auth_realm;
        keep_alive_force_close = 1;        } else {
            /* Expect: 100-continue (RFC 9110 10.1.1): the client holds the
             * body until granted. The stall happens in the body read below,
             * so grant here - just before it, and only when a body is
             * actually expected and not already fully buffered (no continue
             * for 413: the client gets the final status instead). */
            if ((request.content_length > 0 || request.chunked) &&
                strncasecmp(request.expect, "100-continue", 12) == 0 &&
                response.status_code == 0) {
                /* chunked never arrives "fully buffered" (the size lines
                 * keep coming), so it always grants the continue; the CL
                 * path keeps its not-already-buffered condition */
                if (request.chunked || total - header_end_len < request.content_length) {
                    send_continue(client_fd);
                }
            }
            /* Framing sanity first (RFC 9110 6.1 / 8.7): a request that
             * names both Content-Length and Transfer-Encoding is unframeable
             * (classic smuggling shape) — reject with 400. A request TE we
             * don't implement (anything but the final "chunked") gets 501.
             * Both close the connection: the framing state is poisoned. */
            if (request.chunked && request.content_length > 0) {
                set_error_response(&response, 400, "Bad Request");
                keep_alive_force_close = 1;
            } else if (request.te_unsupported && !request.chunked) {
                set_error_response(&response, 501, "Not Implemented");
                keep_alive_force_close = 1;
            } else if (request.chunked) {
                /* RFC 9110 8.7.1: decode the chunked stream in place. The
                 * body is re-assembled contiguously in a fresh buffer (the
                 * raw wire bytes are chunk metadata + payload interleaved),
                 * capped at MAX_REQUEST_SIZE like the CL path. */
                char *body = malloc(MAX_REQUEST_SIZE + 1);
                int blen = 0, ok = 1;
                if (!body) {
                    set_error_response(&response, 500, "Internal Server Error");
                } else {
                    /* p walks the undecoded stream: what we already recv'd
                     * past the headers, plus whatever keeps arriving. */
                    long p = header_end_len;
                    int have = total;
                    for (;;) {
                        /* ensure a full line at p (a chunk-size line) */
                        long ls = p;
                        while (1) {
                            for (; ls < have; ls++) {
                                if (buffer[ls] == '\n') break;
                            }
                            if (ls < have) break;
                            if (deadline && time(NULL) >= deadline) {
                                set_error_response(&response, 408, "Request Timeout");
                                ok = 0;
                                goto chunk_done;
                            }
                            if (have >= (int)sizeof(buffer) - 1) {
                                set_error_response(&response, 431, "Request Header Fields Too Large");
                                ok = 0;
                                goto chunk_done;
                            }
                            struct timeval tv = {req_to > 0 ? req_to : 300, 0};
                            fd_set rfds;
                            FD_ZERO(&rfds);
                            FD_SET(client_fd, &rfds);
                            if (select(client_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
                                set_error_response(&response, 408, "Request Timeout");
                                ok = 0;
                                goto chunk_done;
                            }
                            int n = recv(client_fd, buffer + have, sizeof(buffer) - 1 - have, 0);
                            if (n <= 0) {
                                set_error_response(&response, 400, "Bad Request");
                                ok = 0;
                                goto chunk_done;
                            }
                            have += n;
                        }
                        /* parse "HEX[;ext]" up to the \n */
                        char *ls2 = buffer + p;
                        long sz = strtol(ls2, NULL, 16);
                        /* skip to just past the size line */
                        p = ls + 1;
                        if (sz < 0) {
                            set_error_response(&response, 400, "Bad Request");
                            ok = 0;
                            goto chunk_done;
                        }
                        if (sz == 0) {
                            /* terminating chunk; swallow the trailer block
                             * up to the blank line (RFC 9110 8.7.1). */
                            for (;;) {
                                long ts = p;
                                int done = 0;
                                while (1) {
                                    for (; ts < have; ts++) {
                                        if (buffer[ts] == '\n') {
                                            /* lone line = end of trailers */
                                            if ((ts == p) ||
                                                (ts == p + 1 && buffer[p] == '\r')) {
                                                done = 1;
                                            }
                                            break;
                                        }
                                    }
                                    if (done || ts < have) break;
                                    if (deadline && time(NULL) >= deadline) {
                                        set_error_response(&response, 408, "Request Timeout");
                                        ok = 0;
                                        goto chunk_done;
                                    }
                                    if (have >= (int)sizeof(buffer) - 1) {
                                        set_error_response(&response, 431, "Request Header Fields Too Large");
                                        ok = 0;
                                        goto chunk_done;
                                    }
                                    struct timeval tv = {req_to > 0 ? req_to : 300, 0};
                                    fd_set rfds;
                                    FD_ZERO(&rfds);
                                    FD_SET(client_fd, &rfds);
                                    if (select(client_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
                                        set_error_response(&response, 408, "Request Timeout");
                                        ok = 0;
                                        goto chunk_done;
                                    }
                                    int n = recv(client_fd, buffer + have, sizeof(buffer) - 1 - have, 0);
                                    if (n <= 0) {
                                        set_error_response(&response, 400, "Bad Request");
                                        ok = 0;
                                        goto chunk_done;
                                    }
                                    have += n;
                                }
                                if (done) break;
                                p = ts + 1;
                            }
                            break; /* chunked body complete */
                        }
                        /* read sz payload bytes + CRLF into the body */
                        if (blen + sz > MAX_REQUEST_SIZE) {
                            set_error_response(&response, 413, "Request Entity Too Large");
                            ok = 0;
                            goto chunk_done;
                        }
                        long need = sz + 2; /* data + CRLF */
                        while (have - p < need) {
                            if (deadline && time(NULL) >= deadline) {
                                set_error_response(&response, 408, "Request Timeout");
                                ok = 0;
                                goto chunk_done;
                            }
                            if (have >= (int)sizeof(buffer) - 1) {
                                set_error_response(&response, 413, "Request Entity Too Large");
                                ok = 0;
                                goto chunk_done;
                            }
                            struct timeval tv = {req_to > 0 ? req_to : 300, 0};
                            fd_set rfds;
                            FD_ZERO(&rfds);
                            FD_SET(client_fd, &rfds);
                            if (select(client_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
                                set_error_response(&response, 408, "Request Timeout");
                                ok = 0;
                                goto chunk_done;
                            }
                            int n = recv(client_fd, buffer + have, sizeof(buffer) - 1 - have, 0);
                            if (n <= 0) {
                                set_error_response(&response, 400, "Bad Request");
                                ok = 0;
                                goto chunk_done;
                            }
                            have += n;
                        }
                        memcpy(body + blen, buffer + p, (size_t)sz);
                        blen += (int)sz;
                        p += need;
                    }
                chunk_done:
                    if (ok) {
                        body[blen] = '\0';
                        request.body = body;
                        request.content_length = blen; /* downstream sees a
                            plain CL-framed body: CGI stdin, chat parse and
                            the FCGI relay are all untouched */
                    } else {
                        free(body);
                        keep_alive_force_close = 1;
                    }
                }
            } else if (request.body == NULL && response.status_code == 0) {
                int body_start = header_end_len;
                int have = total - body_start;
                if (have < request.content_length) {
                    int avail = (int)sizeof(buffer) - 1 - total;
                    if (request.content_length - have > avail) {
                        set_error_response(&response, 413, "Request Entity Too Large");
                        /* Body did not fit the single read buffer: the
                         * remaining bytes are still on the socket. If we
                         * kept the connection alive the client's trailing
                         * body would be parsed as the next request
                         * (keep-alive desync / request smuggling). Force a
                         * close so the client must resend on a fresh
                         * connection. */
                        keep_alive_force_close = 1;
                    } else {
                        while (have < request.content_length) {
                            if (deadline && time(NULL) >= deadline) {
                                set_error_response(&response, 408, "Request Timeout");
                                break;
                            }
                            struct timeval tv = {req_to > 0 ? req_to : 300, 0};
                            fd_set rfds;
                            FD_ZERO(&rfds);
                            FD_SET(client_fd, &rfds);
                            if (select(client_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
                                set_error_response(&response, 408, "Request Timeout");
                                break;
                            }
                            int n = recv(client_fd, buffer + total, sizeof(buffer) - 1 - total, 0);
                            if (n <= 0) break;
                            total += n;
                            have += n;
                        }
                    }
                }
                /* Runs for framed and body-less requests alike: the
                 * pipelining bookkeeping below is what carries a subsequent
                 * request forward, so a zero-length body must not skip it.
                 * Do NOT add a `request.body != NULL` clause here — this
                 * branch is only reached while body is still NULL, so such a
                 * guard is never true and silently drops every POST body
                 * (CGI stdin empty, chat endpoint unable to parse). */
                if (have >= request.content_length &&
                    response.status_code == 0) {
                    if (request.content_length > 0) {
                        request.body = malloc(request.content_length + 1);
                        if (request.body) {
                            memcpy(request.body, buffer + body_start, request.content_length);
                            request.body[request.content_length] = '\0';
                        }
                    }
                    /* Everything from here on in buffer belongs to a
                     * subsequent pipelined request: remember its offset so
                     * the next keep-alive iteration can carry it forward
                     * instead of dropping it (HTTP/1.1 pipelining). */
                    prev_consumed = (long)header_end_len + request.content_length;
                    carry = total - (int)prev_consumed;
                }
            }

        /* Native C chat endpoint (src/llm.c): the LLM data path streams
         * SSE straight from this process (forked curl upstream), taking
         * priority over the /react FastCGI relay even when -R is set. */
        if (g_vite_upstream_port > 0 &&
            is_websocket_upgrade(buffer, (size_t)header_end_len)) {
            /* HMR WebSocket: splice to the internal Vite server. */
            ws_tunnel(client_fd, buffer, (size_t)header_end_len);
            return;
        }
        if (g_vite_upstream_port > 0 && is_vite_proxy_route(&request)) {
            proxy_to_vite(client_fd, buffer, (size_t)header_end_len, &request);
            return;
        }
        if (llm_is_chat_route(request.path, request.method)) {
            llm_handle_chat(&request, &response, client_fd);
        } else if (g_react_sock[0] &&
            (strncmp(request.path, "/react", 6) == 0 &&
             (request.path[6] == '\0' || request.path[6] == '/'))) {
            const char *ip = client_addr ? inet_ntoa(client_addr->sin_addr) : "-";
            int status = forward_to_fcgi(g_react_sock, &request, ip, client_fd);
            if (status < 0) {
                /* backend unreachable / protocol error before any byte was
                 * streamed: render our own 502 (forward_to_fcgi streamed
                 * nothing, so this page is the only response). */
                set_error_response(&response, 502, "Bad Gateway");
            } else {
                log_request(ip, &request, status, 0);
                close(client_fd);
                return;
            }
        } else {
            process_request(&request, &response, client_fd);
        }
    }

    /* Streaming dispatchers (SSE chat) already wrote head + body to the
     * socket; log and close — no response serialization, no keep-alive
     * (SSE is close-delimited). */
    if (response.handled) {
        log_request(client_ip, &request, response.status_code, response.body_length);
        free(request.body);
        close(client_fd);
        return;
    }

    if (response.body == NULL && response.stream_path == NULL &&
        response.status_code != 0 && response.status_code != 304) {
        /* Body-less failure statuses get their error page here. 304 is
         * body-less by design (RFC 9110 15.4.5) and must not be replaced. */
        set_error_response(&response, response.status_code, response.status_text);
    }

    if (response.content_type[0] == '\0') {
        strcpy(response.content_type, "text/html");
    }

    int head_only = (strcmp(request.method, "HEAD") == 0);
    /* RFC 7230: HTTP/1.1 is persistent by default; HTTP/1.0 needs an
     * explicit "Connection: keep-alive". An explicit "close" always wins. */
    int keep_alive = (strncmp(request.protocol, "HTTP/1.1", 8) == 0);
    if (request.connection[0]) {
        if (strncasecmp(request.connection, "close", 5) == 0) {
            keep_alive = 0;
        } else if (strncasecmp(request.connection, "keep-alive", 10) == 0) {
            keep_alive = 1;
        }
    }
    if (keep_alive_force_close) {
        keep_alive = 0;
    }
    if (request.chunked) {
        /* Keep-alive pipelining after a chunked body is not decoded for
         * carry-over (the trailer's end offset is awkward to track), so
         * close instead of risking a swallowed request. Valid per HTTP/1.1. */
        keep_alive = 0;
    }
    int response_len;
    int body_sent = build_response(&response, head_only, keep_alive, response_buffer, &response_len);
    {
        int off = 0;
        while (off < response_len) {
            ssize_t sw = send(client_fd, response_buffer + off, (size_t)(response_len - off), 0);
            if (sw <= 0) {
                if (sw < 0 && errno == EINTR) continue;
                keep_alive = 0;
                break;
            }
            off += (int)sw;
        }
    }

    /* stream out-of-buffer bodies (large static files, 206 byte ranges).
     * Zero-copy sendfile(2) on Linux/macOS; fread/send fallback elsewhere.
     * The stream starts at stream_offset (0 = whole file). Note the loop
     * credits only actually-sent bytes, so partial sends no longer drop
     * data the way a fixed-chunk resend would. */
    if (!head_only && response.stream_path) {
        int sent_ok = 0;
#if defined(__APPLE__) || defined(__linux__)
        int sfd = open(response.stream_path, O_RDONLY);
        if (sfd >= 0) {
            off_t off = response.stream_offset;
            size_t remaining = (size_t)response.body_length;
            sent_ok = 1;
            while (remaining > 0) {
#if defined(__APPLE__)
                /* macOS argument order: sendfile(FILE fd, SOCKET fd, ...) */
                off_t chunk_len = (off_t)remaining;
                ssize_t w = sendfile(sfd, client_fd, off, &chunk_len, NULL, 0);
                if (w < 0) {
                    if (errno == EINTR || errno == EAGAIN) {
                        if (chunk_len > 0) { /* partial send: credit and retry */
                            off += chunk_len;
                            remaining -= (size_t)chunk_len;
                        }
                        continue;
                    }
                    sent_ok = 0; /* client gone or file trouble */
                    break;
                }
                off += chunk_len;
                remaining -= (size_t)chunk_len;
#else /* __linux__ */
                ssize_t w = sendfile(client_fd, sfd, &off, remaining);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    sent_ok = 0;
                    break;
                }
                if (w == 0) { /* premature EOF */
                    sent_ok = 0;
                    break;
                }
                remaining -= (size_t)w;
#endif
            }
            close(sfd);
        }
#else
        FILE *file = fopen(response.stream_path, "rb");
        if (file) {
            sent_ok = 1;
            if (response.stream_offset > 0 &&
                fseeko(file, response.stream_offset, SEEK_SET) != 0) {
                sent_ok = 0;
            }
            char chunk[65536];
            size_t remaining = (size_t)response.body_length;
            while (sent_ok && remaining > 0) {
                size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
                size_t n = fread(chunk, 1, want, file);
                if (n == 0) { sent_ok = 0; break; }
                size_t done = 0;
                while (done < n) {
                    ssize_t w = send(client_fd, chunk + done, n - done, 0);
                    if (w <= 0) {
                        if (w < 0 && errno == EINTR) continue;
                        sent_ok = 0;
                        break;
                    }
                    done += (size_t)w;
                }
                remaining -= done;
            }
            fclose(file);
        }
#endif
        if (sent_ok) {
            body_sent += response.body_length;
        } else {
            keep_alive = 0; /* headers promised a body we could not deliver */
        }
    }

    log_request(client_ip, &request, response.status_code, body_sent);

    free(response.body);
    if (response.stream_path) {
        if (response.stream_is_temp) unlink(response.stream_path);
        free(response.stream_path);
    }
    free(request.body);

    served++;
    if (!keep_alive ||
        served >= MAX_KEEPALIVE_REQUESTS ||
        response.status_code >= 500 ||
        g_shutdown_requested) {
        /* shutdown drain: finish this exchange, then close instead of
         * idling on keep-alive - the worker exits after this connection */
        close(client_fd);
        return;
    }
    } /* persistent-connection loop */
}
