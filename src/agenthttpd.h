#ifndef AGENTHTTPD_H
#define AGENTHTTPD_H

/* Public framework API for embedding agent-httpd into a C application.
 *
 * The server binary (src/core/main.c) is itself just a client of this API:
 * it parses argv into an agenthttpd_config and calls agenthttpd_run().
 *
 * Embedding model (pre-fork registration):
 *   1. Fill an agenthttpd_config (zero it first; NULL/0 fields mean default).
 *   2. Register custom routes (agenthttpd_route) and agent tools
 *      (agenthttpd_tool / agenthttpd_tool_exec). Registration is closed once
 *      agenthttpd_run() starts: routes and tools are table snapshots that the
 *      prefork workers inherit, so late registration would be invisible (and
 *      racy) — it is rejected instead.
 *   3. Call agenthttpd_run(&cfg). It blocks for the process lifetime,
 *      handling SIGINT/SIGTERM gracefully; it returns the process exit code.
 *
 * Two extension points:
 *   - Custom routes answer HTTP before the built-in CGI/static dispatch. The
 *     handler fills the HttpResponse and returns 0, or returns -1 to fall
 *     through to the default dispatch. GET/HEAD routes without a body run on
 *     the master's fast event loop; anything else runs on a prefork worker.
 *     Handlers must be stateless: a POST handler executes in a forked worker,
 *     so writes to globals never propagate back.
 *   - Agent tools join the same registry the LLM loop and MCP servers use
 *     (tools.h). agenthttpd_tool registers an in-process C handler;
 *     agenthttpd_tool_exec registers an external process ("exec tool"): the
 *     tool-call arguments go to the command's stdin as JSON, stdout comes
 *     back as the tool result, and the LLM credentials are stripped from the
 *     child's environment — the same isolation rules CGI children get.
 */

#include "httpd.h" /* HttpRequest / HttpResponse */

#include "tools.h" /* ToolFn */

/* ---- configuration (zero-initialized defaults in brackets) ---- */

typedef struct {
    int port;             /* listen port [DEFAULT_PORT] */
    int workers;          /* prefork slow-path pool size; 0 = fork-per-connection [8] */
    const char *docroot;  /* static file root ["./www"] */
    const char *cgi_bin;  /* CGI script directory ["./cgi-bin"] */
    const char *access_log; /* combined-format access log ["./logs/access.log"] */
    const char *htpasswd; /* Basic Auth file; NULL = auth off [NULL] */
    const char *auth_realm; /* 401 realm ["agent-httpd"] */
    int rate_limit_rps;   /* per-IP token-bucket rate (req/s); 0 = off [0] */
    const char *fcgi_socket; /* also serve FastCGI on this UNIX socket; NULL = off [NULL] */
    const char *react_socket; /* resident React SSR backend to relay /react/ to [NULL] */
    int vite_upstream_port; /* DEV ONLY: Vite proxy upstream; 0 = off [0] */
    int no_directory_listing; /* 1 = directory requests answer 404 [0] */
} agenthttpd_config;

/* ---- registration (call before agenthttpd_run; closed after) ---- */

/* Register a custom route. `method` is an exact HTTP verb or "*" for any;
 * `path` is an exact request path, or a prefix when it ends with an
 * asterisk (so a trailing-slash pattern matches everything under it).
 * Query strings are stripped before matching. Returns 0 on success, -1 when
 * the table is full (32), an argument is NULL, or run() already started. */
int agenthttpd_route(const char *method, const char *path,
                     int (*fn)(HttpRequest *request, HttpResponse *response));

/* Register an in-process agent tool (same table as the built-ins and MCP).
 * See tools.h for the ToolFn contract and parameter-schema conventions.
 * Returns 0 on success, -1 on duplicate name / full table (96). */
int agenthttpd_tool(const char *name, const char *desc, const char *params_json,
                    ToolFn fn, void *data);

/* Register an exec tool: `command` runs via /bin/sh -c with the raw JSON
 * tool-call arguments on stdin and stdout as the tool result (capped at
 * 256KB; stderr is folded into the result). The child inherits a scrubbed
 * environment (LLM_API_KEY / LLM_API_URL / LLM_MODEL removed). The command
 * string is developer-supplied configuration — treat it like a crontab
 * line, never build it from model output. Returns 0 on success. */
int agenthttpd_tool_exec(const char *name, const char *desc,
                         const char *params_json, const char *command);

/* ---- lifecycle ---- */

/* Start the server and block until SIGINT/SIGTERM. Returns 0 after a clean
 * drain, 1 on startup failure (bad htpasswd, socket in use, ...). */
int agenthttpd_run(const agenthttpd_config *cfg);

#endif /* AGENTHTTPD_H */
