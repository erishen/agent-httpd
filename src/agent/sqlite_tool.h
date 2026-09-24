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

#endif /* AGENT_SQLITE_TOOL_H */
