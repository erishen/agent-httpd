/* stdio_serve.c — the stdio transport. Protocol contract: stdio_serve.h.
 *
 * The loop is deliberately thin: it parses one JSON request line, builds an
 * HttpRequest, and hands it to the SAME dispatch chain the HTTP listener
 * uses (llm_handle_chat for the chat routes, process_request for everything
 * else). The transport changes the carrier, not the routing.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"
#include "httpd.h"
#include "llm.h"
#include "chatio.h"
#include "minijson.h"
#include "stdio_serve.h"

int g_stdio_mode = 0;

/* The fd that carries the protocol. Claimed by stdio_claim_stdout() before
 * any startup diagnostic is printed. */
static int g_stdio_fd = -1;

int stdio_claim_stdout(void) {
    /* stdout carries the protocol: nothing else may write to it. Dup it and
     * point the process' stdout at stderr, so a .lume script's print() and
     * the framework's own diagnostics land on the supervisor's log instead
     * of corrupting the frame stream. */
    fflush(stdout);
    int proto = dup(STDOUT_FILENO);
    if (proto < 0) {
        proto = STDOUT_FILENO;
    } else {
        (void)dup2(STDERR_FILENO, STDOUT_FILENO);
    }
    g_stdio_fd = proto;
    return proto;
}

/* Cap on the request body one stdio request may carry. The HTTP path caps
 * the body at 1MB and answers 413 above it; the same ceiling here keeps a
 * runaway client from growing the buffer without bound. */
#define STDIO_BODY_MAX (1024 * 1024)

/* Copy the JSON value at *pp into a freshly allocated string.
 *   object/array -> the raw text, verbatim (the route sees exactly the JSON
 *                   the client sent)
 *   string       -> the unescaped content (a body given as a JSON string is
 *                   the raw body text)
 * Returns a malloc'd NUL-terminated string, or NULL when the value is
 * malformed or larger than the cap. */
static char *copy_json_value(const char **pp) {
    const char *p = jws(*pp);
    if (*p == '"') {
        char *out = malloc(STDIO_BODY_MAX + 1);
        if (!out) return NULL;
        const char *q = p;
        if (!jread_string(&q, out, STDIO_BODY_MAX + 1)) {
            free(out);
            return NULL;
        }
        *pp = q;
        return out;
    }
    const char *start = p;
    const char *q = p;
    if (jskip_value(&q) != 0) return NULL;
    size_t n = (size_t)(q - start);
    if (n > (size_t)STDIO_BODY_MAX) return NULL;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, start, n);
    out[n] = '\0';
    *pp = q;
    return out;
}

/* One protocol frame for a non-chat route:
 *   data: {"t":"result","status":<code>,"d":"<body>"}\n\n
 * "d" is always a JSON string (see stdio_serve.h for why). Returns 0, or -1
 * when the peer is gone. */
static int write_result_frame(int fd, int status, const char *body) {
    sbuf b = {0};
    char num[16];
    snprintf(num, sizeof num, "%d", status);
    sb_str(&b, "data: {\"t\":\"result\",\"status\":");
    sb_str(&b, num);
    sb_str(&b, ",\"d\":");
    sb_json_str(&b, body ? body : "");
    sb_str(&b, "}\n\n");
    int rc = 0;
    if (b.oom || !b.p) {
        rc = -1;
    } else if (net_write_all(fd, b.p, b.len) != 0) {
        rc = -1;
    }
    free(b.p);
    return rc;
}

/* Handle one stdio request line. Returns 0 while the peer is still there,
 * -1 once the pipe is gone (the caller stops the loop). */
static int handle_stdio_request(int fd, const char *line) {
    ChatOut out = {fd, 1, 0, 0, {0}};
    int rc = 0;

    char method[16] = "GET";
    const char *v = jfind_value(line, "method");
    if (v && *v == '"') {
        const char *q = v;
        if (!jread_string(&q, method, sizeof method)) method[0] = '\0';
    }

    char path[MAX_PATH_SIZE];
    path[0] = '\0';
    v = jfind_value(line, "path");
    if (v && *v == '"') {
        const char *q = v;
        if (!jread_string(&q, path, sizeof path)) path[0] = '\0';
    }
    if (!path[0]) {
        /* Malformed request: report it and keep the loop alive. The client
         * needs a terminal frame or it would wait for this turn forever. */
        sse_event(&out, "error", "stdio request needs a \"path\" field");
        sse_event(&out, "done", NULL);
        return out.ok ? 0 : -1;
    }

    /* The query string rides on the path, exactly like the HTTP parser does
     * (parse_headers reads the whole request target into path). The DSL
     * bridge splits it back out into req.query / req.query_params. */
    v = jfind_value(line, "query");
    if (v && *v == '"') {
        char query[1024];
        const char *q = v;
        if (jread_string(&q, query, sizeof query) && query[0]) {
            size_t plen = strlen(path);
            size_t qlen = strlen(query);
            if (plen + 1 + qlen < sizeof path) {
                path[plen] = '?';
                memcpy(path + plen + 1, query, qlen + 1);
            }
        }
    }

    /* Body: absent means an empty body, never NULL — the form/JSON handlers
     * dereference request->body (http_route.c form_field). */
    char *body = strdup("");
    v = jfind_value(line, "body");
    if (v) {
        const char *q = v;
        char *parsed = copy_json_value(&q);
        if (parsed) {
            free(body);
            body = parsed;
        }
    }

    HttpRequest req;
    memset(&req, 0, sizeof req);
    set_str(req.method, sizeof req.method, method);
    set_str_utf8(req.path, sizeof req.path, path);
    set_str(req.protocol, sizeof req.protocol, "HTTP/1.1");
    set_str(req.remote_addr, sizeof req.remote_addr, "stdio");
    set_str(req.content_type, sizeof req.content_type, "application/json");
    req.body = body;
    req.content_length = (int)strlen(body);

    HttpResponse resp;
    memset(&resp, 0, sizeof resp);

    if (llm_is_chat_route(req.path, req.method) ||
        llm_is_pse_route(req.path, req.method)) {
        /* The chat handler owns the response: it streams delta/note/error
         * frames to the fd and terminates with its own "done". */
        llm_handle_chat(&req, &resp, fd);
        if (!resp.handled) {
            /* It refused the request before streaming anything (wrong verb,
             * cross-origin rejection). No frame was written, so the client
             * would wait for a "done" that never arrives — say it here. */
            sse_event(&out, "error", resp.status_text);
            sse_event(&out, "done", NULL);
        }
    } else {
        process_request(&req, &resp, fd);
        if (write_result_frame(fd, resp.status_code, resp.body) != 0) rc = -1;
        sse_event(&out, "done", NULL);
    }

    free(resp.body);
    free(req.body);
    if (!out.ok) rc = -1;
    return rc;
}

int agenthttpd_stdio_serve(void) {
    /* stdout was already claimed (and redirected to stderr) by
     * stdio_claim_stdout() before the startup diagnostics ran; fall back to
     * the raw fd if that never happened (direct library use). */
    int proto = g_stdio_fd >= 0 ? g_stdio_fd : stdio_claim_stdout();

    g_stdio_mode = 1;

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) continue; /* blank line: ignore, keep the loop alive */
        if (handle_stdio_request(proto, line) != 0) break; /* peer gone */
    }
    free(line);
    if (proto != STDOUT_FILENO) close(proto);
    return 0;
}
