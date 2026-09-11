#ifndef MCP_H
#define MCP_H

#include "httpd.h"
#include "minijson.h"
#include "tools.h"

/* MCP (Model Context Protocol) stdio client — port of resolve-studio's
 * McpService, C edition.
 *
 * Each configured server runs as a NEWLINE-delimited JSON-RPC subprocess
 * over stdio (MCP stdio transport has no Content-Length framing):
 *   {"jsonrpc":"2.0","id":0,"method":"initialize",...}\n
 *   <line>  -> notifications ignored; responses matched by "id"
 *
 * Concurrency model: the C server is fork-per-worker with long-lived
 * workers, so keeping one persistent MCP child per process would leak
 * across requests. Instead each tools/call spawns a fresh child (spawn ->
 * initialize -> initialized -> tools/call -> SIGKILL). Server startup is
 * ~50-150ms for node-based servers — an acceptable tradeoff for a teaching
 * server, and it guarantees no fd/pid leaks and no cross-worker races.
 * (tools/list happens once in the parent at boot, registering
 * "<serverId>:<toolName>" entries into the shared tool registry.)
 *
 * Security posture: MCP tool *names* never hit the filesystem — they are
 * matched against the table harvested at startup. Arguments are forwarded
 * verbatim to the server process. Approval maps to the server's config
 * field and is logged to stderr (there is no interactive approval channel
 * in the SSE chat path today, so gated tools run with a stderr audit
 * trace; read-only servers are the safe pairing).
 *
 * Config: MCP_SERVERS env = JSON array, or .data/mcp-servers.json
 *   [ { "id":"fs", "transport":"stdio", "command":"npx",
 *       "args":"-y @modelcontextprotocol/server-filesystem /tmp",
 *       "approval":true } ]
 */

#define MCP_SERVERS_MAX 12
#define MCP_ARGS_MAX 24
#define MCP_TOOLS_MAX 48

typedef struct {
    char id[32];
    char argv[MCP_ARGS_MAX][MAX_PATH_SIZE]; /* argv[0]=command, rest=args */
    int argc;
    int approval; /* reserved: audit-only in the SSE path */
} McpServerCfg;

typedef struct {
    char mcp_name[TOOL_NAME_MAX + 1]; /* "<serverId>:<toolName>" */
    char server_id[32];
    char tool_name[48];
    char desc[TOOL_DESC_MAX + 1];
    char props[TOOL_PARAMS_MAX + 1]; /* inputSchema.properties JSON ("{}") */
} McpToolInfo;

/* Parse config, spawn each server once for tools/list, register every
 * harvested tool into the tools registry. Call once in the PARENT before
 * workers fork. Returns the number of MCP tools registered (0 if none). */
int mcp_init(void);

int mcp_server_count(void);
const McpServerCfg *mcp_server(int i);
int mcp_tool_count(void);
const McpToolInfo *mcp_tool(int i);

/* Execute an MCP tool by its registered "<id>:<tool>" name (fresh spawn).
 * Appends a readable text result (content[].text joined, structuredContent
 * pretty-printed via jq(1) when present) or an error description. */
void mcp_call(const char *mcp_name, const char *args_json, sbuf *result);

#endif /* MCP_H */