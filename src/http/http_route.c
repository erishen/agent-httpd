/* Route classification + dispatch: decide, for a parsed request, whether it
 * can run on the fast in-process path (is_fast_request), whether it is a
 * dev-mode Vite proxy target (is_vite_proxy_route), whether its header block
 * is a WebSocket upgrade (is_websocket_upgrade), and how a non-streaming
 * request maps to static file / CGI / health / metrics (process_request).
 * The keep-alive connection loop that drives these lives in src/http.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "internal.h"
#include "httpd.h"
#include "llm.h"
#include "metrics.h"

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

    /* Framework layer: routes registered through agenthttpd_route() run
     * before the built-in method gate, so a custom route may serve any verb
     * (POST included, without needing the CGI path form). A handler returns
     * 0 = handled; -1 = fall through to the dispatch below. */
    if (framework_route_dispatch(request, response)) return 0;

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
