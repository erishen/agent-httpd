#ifndef ROUTER_H
#define ROUTER_H

/* llm-router catalog sync (optional).
 *
 * The chat agent is a tools-sending OpenAI client: when it passes its own
 * `tools` schema the llm-router merely proxies to the upstream model and
 * executes nothing itself — its builtin executor only kicks in for clients
 * that send NO tools. So the router's capability catalog has to be pulled
 * down and made real locally:
 *
 *   GET /v1/mcps    - which stdio MCP servers the router mounts. The router
 *                     view intentionally hides command/env, so spawn config
 *                     for the well-known servers (fs, think, memory) comes
 *                     from a built-in table (npx @modelcontextprotocol/+).
 *   GET /v1/skills  - skill library list; bodies fetched per-skill and
 *                     materialized under skills/router/<name>/SKILL.md so
 *                     the local skills index + skill-run tool pick them up.
 *
 * Both only happen when ROUTER_API_URL (fallback: LLM_API_URL) actually
 * answers like a router; otherwise the call degrades to a silent no-op so
 * a direct-to-provider LLM_API_URL keeps working untouched.
 */
void router_sync_all(void);

#endif /* ROUTER_H */