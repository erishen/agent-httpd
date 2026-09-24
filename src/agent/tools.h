#ifndef TOOLS_H
#define TOOLS_H

#include "minijson.h"

/* Agent tool registry (OpenAI function-calling dialect), src/agent/tools.c.
 *
 * Built-ins are registered at tools_init(); MCP servers register their
 * tools dynamically at startup in the parent (tools_register) under
 * "<serverId>:<toolName>" names. tools_schema_json() builds the request
 * body's "tools" array from the live table (cached; registration only
 * happens in the parent before workers fork, so workers see a frozen
 * schema). tool execution is pure fork+exec / bounded computation: no new
 * library dependencies besides libc (+ jq(1) for MCP pretty-printing).
 *
 * Session-aware tools (remember/recall) receive the current sessionId via
 * tools_dispatch and persist to .data via src/agent/session.c.
 */

#define TOOL_NAME_MAX 64
#define TOOL_DESC_MAX 160
#define TOOL_PARAMS_MAX 4096
#define TOOL_MAX 96

typedef void (*ToolFn)(void *data, const char *args_json,
                       const char *session_id, sbuf *out);

typedef struct {
    char name[TOOL_NAME_MAX + 1];
    char desc[TOOL_DESC_MAX + 1];
    char params[TOOL_PARAMS_MAX + 1]; /* parameters object JSON, "{}" fine */
    ToolFn fn;
    void *data; /* opaque; MCP registrations carry their tool-table index */
} ToolDef;

/* Register the built-in tool set (get_time/calc/read_file/fetch_url/
 * skill-run/remember/recall). Call once in the parent before forks. */
void tools_init(void);

/* Profile allow-list predicate (HARNESS_TOOLS_ALLOW). MCP tools (names
 * containing "__") answer "allowed" — they are gated by MCP_ALLOW instead.
 * Exposed so the DSL bridge can skip a demo tool gracefully instead of
 * treating the trim as a hard registration failure. */
int tools_register_allowed(const char *name);

/* Append a dynamic tool (used by the MCP layer). Returns 0 on success,
 * -1 when the table is full / name duplicated. */
int tools_register(const char *name, const char *desc, const char *params_json,
                   ToolFn fn, void *data);

int tools_count(void);
const ToolDef *tools_get(int i);

/* JSON for the request body's "tools" array, covering every registered
 * tool. Static memory valid until the next tools_register call (parent
 * only). */
const char *tools_schema_json(void);

/* Execute tool `name` with its RAW (unparsed) JSON arguments. session_id
 * may be NULL (remember/recall then write the global memory pool).
 * Appends plain text to `result`; returns 0 on success, -1 on unknown
 * tool/exec failure (the caller relays that text upstream regardless). */
int tools_dispatch(const char *name, const char *args_raw,
                   const char *session_id, sbuf *result);

#endif /* TOOLS_H */