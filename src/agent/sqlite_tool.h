/* Native SQLite support for agent-httpd.
 * Declarations shared by tools.c (registration) and llm.c (system prompt
 * injection). Implementation: sqlite_tool.c.
 */
#ifndef AGENT_SQLITE_TOOL_H
#define AGENT_SQLITE_TOOL_H

/* Register sql_query / sql_tables / sql_schema when env SQLITE_DB is set. */
void sqlite_tools_init(void);

/* Introspected schema + data discipline for the chat system prompt
 * ("" when SQLITE_DB is unset or the db file is missing; cached by mtime). */
const char *sqlite_system_extra(void);

/* DSL-level SQL helpers — the same single-statement guardrails, row dump
 * and read-only/read-write opens as the chat tools, but callable from C
 * (Lume's sql_query / sql_write builtins) with a bare statement instead of a
 * JSON tool invocation.
 *   `db` may be NULL (falls back to env SQLITE_DB); on failure 1 is returned
 *   with a message in `err`.
 * sqlite_query_json: 0 on success, `out` receives a JSON array of row maps
 *   (at most ROW_CAP rows, "[]" for empty results).
 * sqlite_write_exec: 0 on success with `*affected` = rows changed (0 for DDL).
 * Both take optional `?` bind parameters: `params`[0..nparams) are bound to
 * placeholders 1..n in order (a NULL entry binds SQL NULL). Values never
 * enter the SQL text, so the read/write guardrails check the statement
 * skeleton only and injection via parameter values is impossible. */
int sqlite_query_json(const char *db, const char *sql, const char **params,
                      int nparams, sbuf *out, char *err, size_t errsz);
int sqlite_write_exec(const char *db, const char *sql, const char **params,
                      int nparams, int *affected, char *err, size_t errsz);

#endif /* AGENT_SQLITE_TOOL_H */
