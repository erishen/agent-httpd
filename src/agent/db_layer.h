/* Database driver layer for agent-httpd.
 *
 * 2026-09-27: 在 SQLite 原生工具之上加一层"驱动表 + scheme 分派"的编译期
 * 插件化骨架:
 *   - SQLite 后端总是编译(scheme: 空 / 普通路径),零第三方依赖;
 *   - PostgreSQL 后端由 WITH_PG=1 编译并链接 libpq(scheme: postgres:// 或
 *     postgresql://);未编译时对 postgres:// 的调用返回明确错误;
 *   - MySQL 后端为插槽: WITH_MYSQL=1 时编译 stub(当前返回"未实现"错误,
 *     不链接任何库),实现就绪后在同一结构内补齐。
 *
 * DSL 层(Lume 的 sql_query / sql_write)经 db_query_json / db_write_exec
 * 进入分派;`dsn` 为 NULL / 空 / 普通文件路径时走 SQLite 默认路径
 * (env SQLITE_DB),与改造前行为完全一致。
 */
#ifndef AGENT_DB_LAYER_H
#define AGENT_DB_LAYER_H

#include "minijson.h"   /* sbuf */

/* ---- 驱动后端接口 ---- */
typedef struct db_backend {
    const char *scheme;   /* "sqlite" / "postgres" / "mysql" */
    int (*open)(const char *dsn, void **conn, char *err, size_t errsz);
    int (*query_json)(void *conn, const char *sql, const char **params,
                      int nparams, sbuf *out, char *err, size_t errsz);
    int (*write_exec)(void *conn, const char *sql, const char **params,
                      int nparams, int *affected, char *err, size_t errsz);
    /* close: 归还/关闭连接。dsn 用于按连接串归还到池(PG 后端复用) */
    void (*close)(const char *dsn, void *conn);
} db_backend;

/* ---- DSL 分派入口(与 sqlite_query_json / sqlite_write_exec 同签名,
 *      首参改为 dsn: 支持 postgres://… / mysql://… 连接串,其余走 SQLite) ---- */
int db_query_json(const char *dsn, const char *sql, const char **params,
                  int nparams, sbuf *out, char *err, size_t errsz);
int db_write_exec(const char *dsn, const char *sql, const char **params,
                  int nparams, int *affected, char *err, size_t errsz);

/* ---- 单语句校验(从 sqlite_tool.c 提升,SQLite 与远端后端共用) ---- */
const char *db_validate_read(const char *sql, char *body, size_t n);
const char *db_validate_write(const char *sql, char *body, size_t n);

#endif /* AGENT_DB_LAYER_H */
