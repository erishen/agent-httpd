/* Embedded agent-httpd: the whole server as a library.
 *
 * Build & run:   make example-run    (serves on :18101)
 *
 * What this demonstrates, in ~100 lines:
 *   1. Embedding — no fork of the CLI binary, no subprocess management:
 *      link bin/libagenthttpd.a, fill a config, call agenthttpd_run().
 *   2. Custom routes — two in-process handlers registered before run():
 *        GET  /api/status   JSON liveness (runs on the master event loop)
 *        POST /api/echo     echoes the request body as JSON (worker pool)
 *        POST /api/tool     dispatches the "wordcount" tool directly, so
 *                           the tool wiring is testable without an LLM
 *   3. Agent tools — an exec tool ("wordcount") whose implementation is an
 *      external Python process: the LLM's tool-call arguments arrive on its
 *      stdin as JSON and its stdout becomes the tool result. Chat with the
 *      agent (POST /react/api/chat with LLM_API_KEY set) and ask it to
 *      count words — it will invoke the Python process.
 *
 * Probes:
 *   curl --noproxy '*' http://127.0.0.1:18101/api/status
 *   curl --noproxy '*' -X POST -d 'hello framework world' \
 *        http://127.0.0.1:18101/api/echo
 *   curl --noproxy '*' -X POST -H 'Content-Type: application/json' \
 *        -d '{"text":"one two three four"}' http://127.0.0.1:18101/api/tool
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agenthttpd.h"
#include "minijson.h"

/* Handlers are stateless: a POST handler runs in a forked worker, so
 * nothing written to globals here would ever propagate back. */

static int status_handler(HttpRequest *request, HttpResponse *response) {
    (void)request;
    const char *body = "{\"ok\":true,\"server\":\"embedded-example\"}\n";
    response->status_code = 200;
    strcpy(response->status_text, "OK");
    strcpy(response->content_type, "application/json");
    response->body = strdup(body);
    response->body_length = response->body ? (int)strlen(body) : 0;
    return 0;
}

static int echo_handler(HttpRequest *request, HttpResponse *response) {
    const char *text = request->body ? request->body : "";
    sbuf out;
    memset(&out, 0, sizeof(out));
    sb_str(&out, "{\"bytes\":");
    char n[16];
    snprintf(n, sizeof n, "%zu", strlen(text));
    sb_str(&out, n);
    sb_str(&out, ",\"text\":");
    sb_json_str(&out, text);
    sb_str(&out, "}\n");
    if (out.oom) {
        free(out.p);
        response->status_code = 500;
        strcpy(response->status_text, "Internal Server Error");
        strcpy(response->content_type, "text/plain");
        response->body = strdup("out of memory\n");
        response->body_length = response->body ? 14 : 0;
        return 0;
    }
    response->status_code = 200;
    strcpy(response->status_text, "OK");
    strcpy(response->content_type, "application/json");
    response->body = out.p;
    response->body_length = (int)out.len;
    return 0;
}

/* Tool probe: run the registered "wordcount" tool through the real dispatch
 * path (tools_dispatch), exactly as the LLM loop would — just without the
 * model in the middle. */
static int tool_probe_handler(HttpRequest *request, HttpResponse *response) {
    sbuf result;
    memset(&result, 0, sizeof(result));
    int rc = tools_dispatch("wordcount", request->body ? request->body : "{}",
                            NULL, &result);
    if (rc != 0) {
        free(result.p);
        response->status_code = 500;
        strcpy(response->status_text, "Internal Server Error");
        strcpy(response->content_type, "text/plain");
        response->body = strdup("tool dispatch failed\n");
        response->body_length = response->body ? 21 : 0;
        return 0;
    }
    response->status_code = 200;
    strcpy(response->status_text, "OK");
    strcpy(response->content_type, "application/json");
    response->body = result.p;
    response->body_length = (int)result.len;
    return 0;
}

int main(void) {
    agenthttpd_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 18101;
    cfg.workers = 4;      /* slow-path pool; 0 would be fork-per-connection */
    cfg.access_log = "./logs/embedded-example.log";

    /* Registration must happen before run(): routes and tools are pre-fork
     * snapshots inherited by the workers. */
    agenthttpd_route("GET", "/api/status", status_handler);
    agenthttpd_route("POST", "/api/echo", echo_handler);
    agenthttpd_route("POST", "/api/tool", tool_probe_handler);

    agenthttpd_tool_exec("wordcount",
        "Count words and characters in the given text.",
        "{\"text\":{\"type\":\"string\",\"description\":\"text to analyze\"}}",
        "python3 examples/tools/wordcount.py");

    fprintf(stderr, "embedded example: /api/status, /api/echo, /api/tool, "
                    "agent tool 'wordcount'\n");
    return agenthttpd_run(&cfg);
}
