/* agent/tools_api.c — HTTP surface for the MCP tool registry.
 *
 * Why this exists: mcp_call() (mcp.c) is only reachable from inside the
 * process (the ReAct loop). A remote caller — e.g. a lume peer node on
 * another machine — has no way to ask "which tools do you have" or "run
 * this one", because the only data endpoint was /react/api/chat. This adds:
 *
 *   GET  /react/api/tools        → {"ok":true,"tools":[{name,server,desc},...]}
 *   POST /react/api/tools/call   → {"ok":true,"result":"..."} | {"ok":false,"error":"..."}
 *
 * Design notes:
 *   - POST-only for the call path, same CSRF reasoning as the chat endpoint
 *     (a state-changing tool behind a GET is triggerable by an <img> tag).
 *   - The GET listing is read-only but still POST-gated? No: listing is
 *     harmless and useful for discovery, so GET is allowed. Origin is still
 *     checked on the POST path.
 *   - Resident connections are reused (see mcp_tools_call_raw): a call after
 *     the first costs one round-trip to the MCP child, not a full spawn.
 *   - Bodies are capped by the existing request-body limit; the result is
 *     capped at MCP_FINAL_MAX by mcp_call itself.
 *   - SECURITY RED LINE: /react/api/tools/call executes arbitrary registered
 *     MCP tools and MUST NEVER be listed in AUTH_PUBLIC_PATHS. The central
 *     gate (http.c) already requires a session or Basic Auth unless the path
 *     is public; if an operator mistakenly adds this path there, the handler
 *     refuses the call fail-closed (see the guard below) so a misconfig can't
 *     silently expose tool execution. Only the read-only GET listing above is
 *     safe to expose. (No session role concept exists — sessions resolve to a
 *     username only — so the gate is "authenticated", not "admin-only"; that
 *     is intentional, since authenticated peer nodes are the callers.)
 */

#include "tools_api.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mcp.h"
#include "core/minijson.h"

/* Same-origin guard, mirroring llm.c's chat_origin_ok: a browser request
 * carries Origin; a non-browser client (our peer node, smoke tests) may omit
 * it. Only reject when an Origin is present AND does not match Host. */
static int origin_ok(const HttpRequest *request) {
    const char *origin = request->origin;
    if (!origin || !origin[0]) return 1;
    const char *o = strstr(origin, "://");
    const char *oh = o ? o + 3 : origin;
    return strcmp(oh, request->host) == 0;
}

int tools_api_is_route(const char *path, const char *method) {
    if (strcmp(path, "/react/api/tools") == 0) {
        return strcmp(method, "GET") == 0;
    }
    if (strcmp(path, "/react/api/tools/call") == 0) {
        return strcmp(method, "POST") == 0;
    }
    return 0;
}

/* Register this path as a reserved /react route so it is never handed to the
 * SSR/FastCGI relay or the SPA fallback. */
int tools_api_is_reserved(const char *path) {
    return strncmp(path, "/react/api/tools", 16) == 0 &&
           (path[16] == '\0' || path[16] == '/');
}

static void reply_json(HttpResponse *response, int status, const char *text,
                       const char *status_text) {
    response->status_code = status;
    strcpy(response->status_text, status_text);
    strcpy(response->content_type, "application/json");
    response->body = strdup(text);
    if (response->body) response->body_length = strlen(response->body);
}

/* JSON-escape helper that reuses minijson's sbuf. */
static void quote(sbuf *b, const char *s) {
    sb_chr(b, '"');
    for (const char *p = s; p && *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            sb_chr(b, '\\');
            sb_chr(b, (char)c);
        } else if (c == '\n') {
            sb_str(b, "\\n");
        } else if (c == '\r') {
            sb_str(b, "\\r");
        } else if (c == '\t') {
            sb_str(b, "\\t");
        } else if (c < 0x20) {
            char tmp[8];
            snprintf(tmp, sizeof tmp, "\\u%04x", c);
            sb_str(b, tmp);
        } else {
            sb_chr(b, (char)c);
        }
    }
    sb_chr(b, '"');
}

int tools_api_handle(const HttpRequest *request, HttpResponse *response) {
    if (strcmp(request->path, "/react/api/tools") == 0) {
        /* ---- listing ---- */
        sbuf out = {0};
        sb_str(&out, "{\"ok\":true,\"tools\":[");
        int n = mcp_tool_count();
        int written = 0;
        for (int i = 0; i < n; i++) {
            const McpToolInfo *t = mcp_tool(i);
            if (!t || !t->mcp_name[0]) continue;
            if (written++) sb_chr(&out, ',');
            sb_str(&out, "{\"name\":");
            quote(&out, t->mcp_name);
            sb_str(&out, ",\"server\":");
            quote(&out, t->server_id);
            if (t->desc[0]) {
                sb_str(&out, ",\"desc\":");
                quote(&out, t->desc);
            }
            sb_chr(&out, '}');
        }
        sb_str(&out, "]}");
        reply_json(response, 200, out.p ? out.p : "{\"ok\":true,\"tools\":[]}", "OK");
        free(out.p);
        return 1;
    }

    /* ---- call ---- */
    /* Defense-in-depth: the call path executes arbitrary registered MCP tools
     * and must never be reachable without authentication. The central gate
     * (http.c) skips auth when the path is listed in AUTH_PUBLIC_PATHS; if an
     * operator adds this path there by mistake, refuse here rather than
     * exposing tool execution to the world (fail-closed). The read-only GET
     * listing above is the only part safe to expose. */
    if (is_public_path(request->path)) {
        reply_json(response, 403,
                   "{\"ok\":false,\"error\":\"tool execution is not available on a public path\"}",
                   "Forbidden");
        return 1;
    }

    if (!origin_ok(request)) {
        reply_json(response, 403, "{\"ok\":false,\"error\":\"cross-origin request rejected\"}",
                   "Forbidden");
        return 1;
    }

    const char *name = jfind_value(request->body, "name");
    if (!name || *name != '"') {
        reply_json(response, 400,
                   "{\"ok\":false,\"error\":\"body must be JSON: {name, arguments?}\"}",
                   "Bad Request");
        return 1;
    }
    char tool[256];
    if (!jread_string(&name, tool, sizeof tool) || !tool[0]) {
        reply_json(response, 400, "{\"ok\":false,\"error\":\"name must be a non-empty string\"}",
                   "Bad Request");
        return 1;
    }

    /* arguments: pass the raw JSON value through untouched (mcp_call feeds it
     * straight into the MCP tools/call params). Absent / non-object → "{}". */
    const char *args = jfind_value(request->body, "arguments");
    /* Absent arguments is legal (the MCP spec allows omitting them): treat it
     * as {} so a zero-arg tool like echo__pong still runs. Only a PRESENT but
     * malformed value is a 400. */
    if (!args) {
        sbuf result = {0};
        mcp_call(tool, "{}", &result);
        sbuf out = {0};
        int failed = (result.p && strncmp(result.p, "error:", 6) == 0);
        sb_str(&out, failed ? "{\"ok\":false,\"error\":" : "{\"ok\":true,\"result\":");
        quote(&out, result.p ? result.p : "");
        sb_str(&out, ",\"tool\":");
        quote(&out, tool);
        sb_chr(&out, '}');
        reply_json(response, failed ? 502 : 200,
                   out.p ? out.p : "{\"ok\":false,\"error\":\"empty\"}",
                   failed ? "Bad Gateway" : "OK");
        free(out.p);
        free(result.p);
        return 1;
    }
    if (*args == '{' || *args == '[') {
        /* measure the value so mcp_call sees exactly one JSON value */
        const char *p = args;
        if (jskip_value(&p) == 0) {
            size_t len = (size_t)(p - args);
            char *copy = malloc(len + 1);
            if (!copy) {
                reply_json(response, 500,
                           "{\"ok\":false,\"error\":\"out of memory\"}",
                           "Internal Server Error");
                return 1;
            }
            memcpy(copy, args, len);
            copy[len] = '\0';
            sbuf result = {0};
            mcp_call(tool, copy, &result);
            free(copy);
            sbuf out = {0};
            /* mcp_call prefixes failures with "error: " — surface them as ok:false
             * so a remote caller can branch without string-sniffing prose. */
            int failed = (result.p && strncmp(result.p, "error:", 6) == 0);
            sb_str(&out, failed ? "{\"ok\":false,\"error\":" : "{\"ok\":true,\"result\":");
            quote(&out, result.p ? result.p : "");
            sb_str(&out, ",\"tool\":");
            quote(&out, tool);
            sb_chr(&out, '}');
            reply_json(response, failed ? 502 : 200,
                       out.p ? out.p : "{\"ok\":false,\"error\":\"empty\"}",
                       failed ? "Bad Gateway" : "OK");
            free(out.p);
            free(result.p);
            return 1;
        }
    }

    reply_json(response, 400,
               "{\"ok\":false,\"error\":\"arguments must be a JSON object or array\"}",
               "Bad Request");
    return 1;
}
