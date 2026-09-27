/* Database driver layer: scheme dispatch + optional remote backends.
 *
 * 2026-09-27: 插件化骨架。SQLite 是默认、总是编译的后端(零第三方依赖);
 * PostgreSQL 后端在 WITH_PG=1 时编译(#ifdef HAVE_LIBPQ)并链接 libpq;
 * MySQL 后端在 WITH_MYSQL=1 时编译(#ifdef HAVE_MYSQL)并链接
 * libmysqlclient(prepared statement + 连接池, 与 PG 后端同模式)。
 * 未编译的 scheme 在运行时返回明确错误, 不会静默落到 SQLite。
 *
 * 安全与 SQLite 后端一致: 单语句 + 白名单校验(db_validate_read / write,
 * 定义在 sqlite_tool.c, 此处复用), 参数经 PQexecParams 绑定,
 * 参数值不进入 SQL 文本, 注入不可能。
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "db_layer.h"
#include "sqlite_tool.h"

#ifdef HAVE_LIBPQ
#include <libpq-fe.h>
#endif

#define SQL_MAX 8192

/* ---- scheme 识别 ---- */

typedef enum { DB_SQLITE, DB_POSTGRES, DB_MYSQL } db_scheme_t;

static db_scheme_t db_scheme(const char *dsn) {
    if (!dsn || !dsn[0]) return DB_SQLITE;
    /* 大小写不敏感前缀匹配; postgres:// 与 postgresql:// 等价 */
    char pre[13];
    size_t n = 0;
    while (dsn[n] && n < sizeof(pre) - 1) {
        pre[n] = (char)tolower((unsigned char)dsn[n]);
        n++;
    }
    pre[n] = '\0';
    if (strncmp(pre, "postgresql://", 13) == 0 ||
        strncmp(pre, "postgres://", 11) == 0)
        return DB_POSTGRES;
    if (strncmp(pre, "mysql://", 8) == 0) return DB_MYSQL;
    return DB_SQLITE;
}

/* 前置声明: 公共 helper(错误去敏/池 key 哈希/JSON 单元格)定义在 PG 块
 * 之后(MySQL 块之前), PG 块在前部调用, 需先声明。默认构建(无远端后端)
 * 不编译这些符号。 */
#if defined(HAVE_LIBPQ) || defined(HAVE_MYSQL)
static void db_err_sanitized(char *err, size_t errsz, const char *label,
                             const char *detail);
static uint64_t fnv1a64(const char *s, size_t n);
static void fnv1a64_hex(const char *s, size_t n, char out[17]);
static void db_json_cell(const char *val, sbuf *out);
#endif

/* ===================== PostgreSQL 后端(WITH_PG=1) ===================== */

#ifdef HAVE_LIBPQ

/* ---- PostgreSQL 进程内连接复用池 ----
 * agent-httpd 是 prefork worker 模型:每个 worker 独立进程,池是进程内
 * 静态链表(无跨进程竞争;worker 内请求串行,每 dsn 同时至多 1 个连接,
 * 故每 dsn 缓存 1 个,全局上限 PG_POOL_CAP 个)。
 * 取出时校验 PQstatus;归还时 RESET ALL 清会话状态;连接失效或池满
 * 直接关闭。框架存在 slow-path 线程池,池操作加互斥锁。 */
#define PG_POOL_CAP 8

typedef struct pg_slot {
    char key[256];      /* 匹配键: 密码段已哈希, 不保留 DSN 明文密码 */
    PGconn *conn;
    struct pg_slot *next;
} pg_slot;

static pg_slot *g_pg_pool = NULL;
static int g_pg_pool_n = 0;
static pthread_mutex_t g_pg_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* PG URI 形如 postgres://user:pass@host:port/db?opts: 把 userinfo 里的
 * 密码段替换为哈希, 其余原样保留(host/port/db/参数都影响连接, 须进 key)。 */
static void pg_pool_key(const char *dsn, char *key, size_t n) {
    const char *scheme = strstr(dsn, "://");
    const char *at = scheme ? strchr(scheme + 3, '@') : NULL;
    if (!at) { snprintf(key, n, "%s", dsn); return; }   /* 无 userinfo, 无密码 */
    const char *colon = memchr(scheme + 3, ':', (size_t)(at - (scheme + 3)));
    if (!colon) { snprintf(key, n, "%s", dsn); return; } /* 只有 user, 无密码 */
    size_t pre = (size_t)(colon - dsn);
    size_t post = (size_t)(at - dsn);   /* '@' 位置 */
    if (pre + 17 + (strlen(dsn) - post) + 1 > n) {
        snprintf(key, n, "%s", dsn);    /* 放不下: 原样(罕见, 保匹配一致) */
        return;
    }
    memcpy(key, dsn, pre);
    key[pre] = ':';
    fnv1a64_hex(colon + 1, (size_t)(at - (colon + 1)), key + pre + 1);
    strcpy(key + pre + 1 + 16, dsn + post);   /* "@host:port/db?opts" */
    key[n - 1] = '\0';
}

/* 取空闲连接;无匹配或连接失效则返回 NULL(调用方 PQconnectdb 新建) */
static PGconn *pg_pool_get(const char *dsn) {
    char key[256];
    pg_pool_key(dsn, key, sizeof key);
    pthread_mutex_lock(&g_pg_pool_lock);
    pg_slot **pp = &g_pg_pool;
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            pg_slot *s = *pp;
            *pp = s->next;
            PGconn *c = s->conn;
            free(s);
            g_pg_pool_n--;
            pthread_mutex_unlock(&g_pg_pool_lock);
            if (PQstatus(c) == CONNECTION_OK) return c;
            PQfinish(c);            /* 连接已失效,丢弃由调用方重建 */
            return NULL;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_pg_pool_lock);
    return NULL;
}

/* 归还连接;RESET ALL 失败、连接失效或池满时直接关闭不缓存 */
static void pg_pool_put(const char *dsn, PGconn *c) {
    PGresult *r = PQexec(c, "RESET ALL");
    if (r) PQclear(r);
    pthread_mutex_lock(&g_pg_pool_lock);
    int reuse = PQstatus(c) == CONNECTION_OK && g_pg_pool_n < PG_POOL_CAP;
    if (reuse) {
        pg_slot *s = calloc(1, sizeof *s);
        if (s) {
            pg_pool_key(dsn, s->key, sizeof s->key);
            s->conn = c;
            s->next = g_pg_pool;
            g_pg_pool = s;
            g_pg_pool_n++;
        } else {
            reuse = 0;
        }
    }
    pthread_mutex_unlock(&g_pg_pool_lock);
    if (!reuse) PQfinish(c);
}

static int pg_open(const char *dsn, void **conn, char *err, size_t errsz) {
    PGconn *c = pg_pool_get(dsn);
    if (!c) {
        c = PQconnectdb(dsn);
        if (!c) {
            snprintf(err, errsz, "postgres: PQconnectdb failed (out of memory)");
            return 1;
        }
        if (PQstatus(c) != CONNECTION_OK) {
            db_err_sanitized(err, errsz, "postgres: connection failed",
                             PQerrorMessage(c));
            PQfinish(c);
            return 1;
        }
    }
    *conn = c;
    return 0;
}

/* 归还到池(连接失效 / 池满 / RESET 失败时内部直接关闭) */
static void pg_close(const char *dsn, void *conn) {
    pg_pool_put(dsn, (PGconn *)conn);
}

/* 把 SQLite/MySQL 风格的 `?` 占位符改写为 PostgreSQL 的 $1..$n。
 * 朴素扫描: 跳过单引号字符串、双引号标识符与注释, 其余位置的 `?`
 * 按出现顺序编号; 字符串字面量里的 `?` 不被改写(骨架阶段接受的限制,
 * 正常 SQL 字面量里几乎不会出现裸 `?`)。 */
static int pg_convert_qmarks(const char *sql, char *out, size_t n) {
    size_t o = 0;
    int num = 0;
    for (const char *s = sql; *s; ) {
        if (o + 2 >= n) return -1;   /* 输出缓冲区不足 */
        if (*s == '\'' ) {           /* 单引号字符串: '' 为转义 */
            out[o++] = *s++;
            while (*s) {
                out[o++] = *s;
                if (*s == '\'') {
                    s++;
                    if (*s == '\'') { out[o++] = *s++; continue; }
                    break;
                }
                s++;
                if (o >= n) return -1;
            }
        } else if (*s == '"') {      /* 双引号标识符: "" 为转义 */
            out[o++] = *s++;
            while (*s) {
                out[o++] = *s;
                if (*s == '"') {
                    s++;
                    if (*s == '"') { out[o++] = *s++; continue; }
                    break;
                }
                s++;
                if (o >= n) return -1;
            }
        } else if (*s == '-' && s[1] == '-') {       /* 行注释 */
            while (*s && *s != '\n') out[o++] = *s++;
        } else if (*s == '/' && s[1] == '*') {       /* 块注释 */
            out[o++] = *s++; out[o++] = *s++;
            while (*s && !(*s == '*' && s[1] == '/')) {
                out[o++] = *s++;
                if (o >= n) return -1;
            }
            if (*s) { out[o++] = *s++; out[o++] = *s++; }
        } else if (*s == '?') {
            num++;
            int written = snprintf(out + o, n - o, "$%d", num);
            if (written < 0 || (size_t)written >= n - o) return -1;
            o += (size_t)written;
            s++;
        } else {
            out[o++] = *s++;
        }
    }
    out[o] = '\0';
    return num;
}

/* 文本模式下按值外观推断 JSON 类型: NULL→null, 纯数字→number,
 * t/f→bool(PG 布尔文本), 其余→字符串。 */
static int pg_query_json(void *conn, const char *sql, const char **params,
                         int nparams, sbuf *out, char *err, size_t errsz) {
    char body[SQL_MAX];
    const char *verr = db_validate_read(sql, body, sizeof body);
    if (verr) { snprintf(err, errsz, "%s", verr); return 1; }
    char converted[SQL_MAX];
    if (pg_convert_qmarks(body, converted, sizeof converted) < 0) {
        snprintf(err, errsz, "postgres: SQL too large after placeholder rewrite");
        return 1;
    }
    PGresult *res = PQexecParams((PGconn *)conn, converted, nparams, NULL,
                                 (const char *const *)params, NULL, NULL, 0);  /* 文本结果格式,类型按值外观推断 */
    if (!res) {
        db_err_sanitized(err, errsz, "postgres: query failed",
                         PQerrorMessage((PGconn *)conn));
        return 1;
    }
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        db_err_sanitized(err, errsz, "postgres: query failed",
                         PQresultErrorMessage(res));
        PQclear(res);
        return 1;
    }
    int rows = PQntuples(res), cols = PQnfields(res);
    int row_cap = 200;
    if (rows > row_cap) rows = row_cap;
    sb_str(out, "[");
    for (int r = 0; r < rows; r++) {
        if (r) sb_str(out, ",");
        sb_str(out, "{");
        for (int c = 0; c < cols; c++) {
            if (c) sb_str(out, ",");
            char key[192];
            snprintf(key, sizeof key, "\"%s\":",
                     PQfname(res, c) ? PQfname(res, c) : "col");
            sb_str(out, key);
            db_json_cell(PQgetisnull(res, r, c) ? NULL
                                                  : PQgetvalue(res, r, c),
                         out);
        }
        sb_str(out, "}");
    }
    sb_str(out, "]");
    PQclear(res);
    return 0;
}

static int pg_write_exec(void *conn, const char *sql, const char **params,
                         int nparams, int *affected, char *err, size_t errsz) {
    char body[SQL_MAX];
    const char *verr = db_validate_write(sql, body, sizeof body);
    if (verr) { snprintf(err, errsz, "%s", verr); return 1; }
    char converted[SQL_MAX];
    if (pg_convert_qmarks(body, converted, sizeof converted) < 0) {
        snprintf(err, errsz, "postgres: SQL too large after placeholder rewrite");
        return 1;
    }
    PGresult *res = PQexecParams((PGconn *)conn, converted, nparams, NULL,
                                 (const char *const *)params, NULL, NULL, 0);  /* 文本结果格式,类型按值外观推断 */
    if (!res) {
        db_err_sanitized(err, errsz, "postgres: query failed",
                         PQerrorMessage((PGconn *)conn));
        return 1;
    }
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        db_err_sanitized(err, errsz, "postgres: query failed",
                         PQresultErrorMessage(res));
        PQclear(res);
        return 1;
    }
    const char *tuples = PQcmdTuples(res);
    *affected = tuples ? (int)atoll(tuples) : 0;
    PQclear(res);
    return 0;
}

#endif /* HAVE_LIBPQ */

#if defined(HAVE_LIBPQ) || defined(HAVE_MYSQL)
/* ---- 远端 DB 错误去敏(两后端共用) ----
 * libpq / libmysqlclient 的错误原文可能回显 SQL 片段与连接元数据
 * (host/port/user): 详细原文只进 stderr(服务端调试通道), DSL 侧只拿
 * 简短类别, 防查询内容经 vm error 流向外泄(500 页面、tool 回传、脚本日志)。 */
static void db_err_sanitized(char *err, size_t errsz, const char *label,
                             const char *detail) {
    fprintf(stderr, "[db] %s: %s\n", label, detail ? detail : "unknown error");
    snprintf(err, errsz, "%s", label);
}

/* 连接串匹配键: 密码段替换为 FNV-1a64 哈希, 进程内存不保留 DSN 明文密码。
 * 密码不同 → 哈希不同 → 各自连接, 不会误复用旧密码的连接。 */
static uint64_t fnv1a64(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void fnv1a64_hex(const char *s, size_t n, char out[17]) {
    snprintf(out, 17, "%016llx", (unsigned long long)fnv1a64(s, n));
}

/* ---- 文本值外观推断(远端后端共用; 默认构建不编译, 避免 unused 告警) ----
 * "t"/"f" → bool; 纯数字(含负号/小数点/指数) → 原样输出; NULL → null;
 * 其余 → JSON 字符串。PG 文本结果与 MySQL 按字符串取的列都走这里。 */
static void db_json_cell(const char *val, sbuf *out) {
    if (!val) { sb_str(out, "null"); return; }
    if (strcmp(val, "t") == 0) { sb_str(out, "true"); return; }
    if (strcmp(val, "f") == 0) { sb_str(out, "false"); return; }
    char *end = NULL;
    strtod(val, &end);
    if (end != val && *end == '\0') {   /* 纯数字 */
        sb_str(out, val);
        return;
    }
    sb_json_str(out, val);
}
#endif /* HAVE_LIBPQ || HAVE_MYSQL */


/* ===================== MySQL 后端(WITH_MYSQL=1) =====================
 * 基于 libmysqlclient prepared statement:
 *   - 参数绑定: DSL 参数为文本, 按值外观推断 LONGLONG / DOUBLE / STRING;
 *   - 占位符 `?` 由 MySQL 服务器解析, 无需本地转换(与 PG 的 $n 不同);
 *   - 输出读取: mysql_stmt_fetch_column 统一按字符串取, 类型由
 *     db_json_cell 按值外观推断(与 PG 后端同策略);
 *   - 连接池与 PG 同模式: 进程内静态链表, 每 dsn 1 个,
 *     全局上限 MYSQL_POOL_CAP。 */

#ifdef HAVE_MYSQL

#include <errno.h>
#include <mysql.h>
#include <stdbool.h>

#define MYSQL_POOL_CAP 8

static void mysql_backend_close(const char *dsn, void *conn);   /* 驱动表 close 前置声明 */

typedef struct mysql_slot {
    char key[256];      /* 匹配键: 密码段已哈希, 不保留 DSN 明文密码 */
    MYSQL *conn;
    struct mysql_slot *next;
} mysql_slot;

static mysql_slot *g_mysql_pool = NULL;
static int g_mysql_pool_n = 0;
static pthread_mutex_t g_mysql_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* DSN 形如 mysql://user[:pass]@host[:port][/db], 各部分可缺省 */
static void mysql_parse_dsn(const char *dsn, char *user, size_t usz,
                            char *pass, size_t psz, char *host, size_t hsz,
                            unsigned int *port, char *db, size_t dsz) {
    user[0] = pass[0] = host[0] = db[0] = '\0';
    *port = 3306;
    const char *rest = strstr(dsn, "://");
    if (!rest) return;
    rest += 3;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", rest);
    char *slash = strchr(tmp, '/');
    if (slash) { snprintf(db, dsz, "%s", slash + 1); *slash = '\0'; }
    char *at = strchr(tmp, '@');
    if (at) {
        *at = '\0';
        char *colon = strchr(tmp, ':');
        if (colon) {
            *colon = '\0';
            snprintf(user, usz, "%s", tmp);
            snprintf(pass, psz, "%s", colon + 1);
        } else {
            snprintf(user, usz, "%s", tmp);
        }
        snprintf(host, hsz, "%s", at + 1);
    } else {
        snprintf(host, hsz, "%s", tmp);
    }
    char *hc = strchr(host, ':');
    if (hc) {
        *port = (unsigned int)atoi(hc + 1);
        *hc = '\0';
    }
}

static MYSQL *mysql_connect_dsn(const char *dsn, char *err, size_t errsz) {
    char user[128], pass[128], host[128], db[128];
    unsigned int port;
    mysql_parse_dsn(dsn, user, sizeof user, pass, sizeof pass,
                    host, sizeof host, &port, db, sizeof db);
    MYSQL *m = mysql_init(NULL);
    if (!m) { snprintf(err, errsz, "mysql: mysql_init failed"); return NULL; }
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(m, host[0] ? host : "localhost",
                            user[0] ? user : NULL,
                            pass[0] ? pass : NULL,
                            db[0] ? db : NULL, port, NULL, 0)) {
        db_err_sanitized(err, errsz, "mysql: connection failed",
                         mysql_error(m));
        mysql_close(m);
        return NULL;
    }
    return m;
}

/* 池匹配键: 密码段替换为 FNV-1a64 哈希, 进程内存不保留 DSN 明文密码。
 * user@host:port:db 与密码哈希共同决定连接复用(密码不同 → 键不同)。 */
static void mysql_pool_key(const char *dsn, char *key, size_t n) {
    char user[128], pass[128], host[128], db[128];
    unsigned int port;
    mysql_parse_dsn(dsn, user, sizeof user, pass, sizeof pass,
                    host, sizeof host, &port, db, sizeof db);
    if (pass[0])
        snprintf(key, n, "%s@%s:%u:%s:%016llx", user, host, port, db,
                 (unsigned long long)fnv1a64(pass, strlen(pass)));
    else
        snprintf(key, n, "%s@%s:%u:%s", user, host, port, db);
}

/* 取空闲连接; 无匹配或连接失效则返回 NULL(调用方新建) */
static MYSQL *mysql_pool_get(const char *dsn) {
    char key[256];
    mysql_pool_key(dsn, key, sizeof key);
    pthread_mutex_lock(&g_mysql_pool_lock);
    mysql_slot **pp = &g_mysql_pool;
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            mysql_slot *s = *pp;
            *pp = s->next;
            MYSQL *m = s->conn;
            free(s);
            g_mysql_pool_n--;
            pthread_mutex_unlock(&g_mysql_pool_lock);
            if (mysql_ping(m) == 0) return m;
            mysql_close(m);
            return NULL;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_mysql_pool_lock);
    return NULL;
}

/* 归还连接; 连接失效 / reset 会话失败 / 池满时直接关闭 */
static void mysql_pool_put(const char *dsn, MYSQL *m) {
    if (mysql_ping(m) != 0) { mysql_close(m); return; }
    if (mysql_reset_connection(m) != 0) { mysql_close(m); return; }
    pthread_mutex_lock(&g_mysql_pool_lock);
    int reuse = g_mysql_pool_n < MYSQL_POOL_CAP;
    if (reuse) {
        mysql_slot *s = calloc(1, sizeof *s);
        if (s) {
            mysql_pool_key(dsn, s->key, sizeof s->key);
            s->conn = m;
            s->next = g_mysql_pool;
            g_mysql_pool = s;
            g_mysql_pool_n++;
        } else reuse = 0;
    }
    pthread_mutex_unlock(&g_mysql_pool_lock);
    if (!reuse) mysql_close(m);
}

static int mysql_open(const char *dsn, void **conn, char *err, size_t errsz) {
    MYSQL *m = mysql_pool_get(dsn);
    if (!m) {
        m = mysql_connect_dsn(dsn, err, errsz);
        if (!m) return 1;
    }
    *conn = m;
    return 0;
}

/* 参数绑定: 文本按外观推断 LONGLONG / DOUBLE / STRING */
/* 参数绑定: 文本按外观推断 LONGLONG / DOUBLE / STRING。
 * 注意: libmysqlclient 的 bind_param 引用调用方 buffer, 值数组必须存活
 * 到 mysql_stmt_execute 之后(execute 前释放会读到堆垃圾), 因此调用方
 * 持有 b/il/fl 直到执行完成再释放。 */
static void mysql_fill_bind(MYSQL_BIND *b, long long *il, double *fl,
                            const char **params, int nparams) {
    for (int i = 0; i < nparams; i++) {
        const char *t = params[i] ? params[i] : "";
        char *end = NULL;
        errno = 0;
        long long v = strtoll(t, &end, 10);
        if (end != t && *end == '\0' && errno != ERANGE) {
            b[i].buffer_type = MYSQL_TYPE_LONGLONG;
            il[i] = v;
            b[i].buffer = &il[i];
        } else {
            end = NULL;
            errno = 0;
            double d = strtod(t, &end);
            if (end != t && *end == '\0' && errno != ERANGE) {
                b[i].buffer_type = MYSQL_TYPE_DOUBLE;
                fl[i] = d;
                b[i].buffer = &fl[i];
            } else {
                b[i].buffer_type = MYSQL_TYPE_STRING;
                b[i].buffer = (void *)t;
                b[i].buffer_length = (unsigned long)strlen(t);
            }
        }
    }
}

static int mysql_query_json(void *conn, const char *sql, const char **params,
                            int nparams, sbuf *out, char *err, size_t errsz) {
    char body[SQL_MAX];
    const char *verr = db_validate_read(sql, body, sizeof body);
    if (verr) { snprintf(err, errsz, "%s", verr); return 1; }
    MYSQL *m = (MYSQL *)conn;
    MYSQL_STMT *stmt = mysql_stmt_init(m);
    if (!stmt) { snprintf(err, errsz, "mysql: mysql_stmt_init failed"); return 1; }
    int rc = 0;
    MYSQL_BIND *pb = NULL;
    long long *il = NULL;
    double *fl = NULL;
    MYSQL_RES *meta = NULL;
    unsigned int cols = 0;
    my_ulonglong total = 0;
    MYSQL_BIND *ob = NULL;
    unsigned long *lens = NULL;
    bool *nulls = NULL;
    size_t *caps = NULL;
    char **bufs = NULL;
    if (mysql_stmt_prepare(stmt, body, (unsigned long)strlen(body)) != 0) {
        db_err_sanitized(err, errsz, "mysql: prepare failed",
                         mysql_stmt_error(stmt));
        rc = 1; goto done;
    }
    if (nparams > 0) {
        pb = calloc((size_t)nparams, sizeof *pb);
        il = calloc((size_t)nparams, sizeof *il);
        fl = calloc((size_t)nparams, sizeof *fl);
        if (!pb || !il || !fl) {
            snprintf(err, errsz, "mysql: out of memory binding params");
            rc = 1; goto done;
        }
        mysql_fill_bind(pb, il, fl, params, nparams);
        if (mysql_stmt_bind_param(stmt, pb) != 0) {
            db_err_sanitized(err, errsz, "mysql: bind params failed",
                             mysql_stmt_error(stmt));
            rc = 1; goto done;
        }
    }
    if (mysql_stmt_execute(stmt) != 0) {
        db_err_sanitized(err, errsz, "mysql: query failed",
                         mysql_stmt_error(stmt));
        rc = 1; goto done;
    }
    if (mysql_stmt_store_result(stmt) != 0) {
        db_err_sanitized(err, errsz, "mysql: query failed",
                         mysql_stmt_error(stmt));
        rc = 1; goto done;
    }
    meta = mysql_stmt_result_metadata(stmt);
    cols = meta ? mysql_num_fields(meta) : 0;
    total = mysql_stmt_num_rows(stmt);
    if (total > 200) total = 200;
    /* 绑定每列输出缓冲(MYSQL_TYPE_STRING: 服务器端转文本), 一次 fetch 全列。
     * fetch_column 逐列取的模式在新版 libmysqlclient 下不可靠(未绑定列时
     * fetch 定位行为不一致), 标准做法是 bind_result 后 fetch 填充。 */
    ob = cols ? calloc(cols, sizeof *ob) : NULL;
    lens = cols ? calloc(cols, sizeof *lens) : NULL;
    nulls = cols ? calloc(cols, sizeof *nulls) : NULL;
    caps = cols ? calloc(cols, sizeof *caps) : NULL;
    bufs = cols ? calloc(cols, sizeof *bufs) : NULL;
    if (cols && (!ob || !lens || !nulls || !caps || !bufs)) {
        snprintf(err, errsz, "mysql: out of memory for result buffers");
        rc = 1; goto done;
    }
    for (unsigned int c = 0; c < cols; c++) {
        MYSQL_FIELD *f = mysql_fetch_field_direct(meta, c);
        size_t cap = 4096;
        if (f && f->max_length > (unsigned long)cap) cap = f->max_length + 1;
        bufs[c] = malloc(cap);
        if (!bufs[c]) {
            snprintf(err, errsz, "mysql: out of memory for result buffers");
            rc = 1; goto done;
        }
        caps[c] = cap;
        ob[c].buffer_type = MYSQL_TYPE_STRING;
        ob[c].buffer = bufs[c];
        ob[c].buffer_length = (unsigned long)cap;
        ob[c].length = &lens[c];
        ob[c].is_null = &nulls[c];
    }
    int brc = cols ? mysql_stmt_bind_result(stmt, ob) : 0;
    sb_str(out, "[");
    if (brc == 0) {
        for (my_ulonglong r = 0; r < total; r++) {
            int frc = mysql_stmt_fetch(stmt);
            if (frc != 0) break;   /* MYSQL_NO_DATA(取完)或其他错误 */
            if (r) sb_str(out, ",");
            sb_str(out, "{");
            for (unsigned int c = 0; c < cols; c++) {
                if (c) sb_str(out, ",");
                MYSQL_FIELD *f = mysql_fetch_field_direct(meta, c);
                char key[192];
                snprintf(key, sizeof key, "\"%s\":", f ? f->name : "col");
                sb_str(out, key);
                if (nulls[c]) {
                    sb_str(out, "null");
                } else {
                    unsigned long blen = lens[c];
                    if (blen >= caps[c]) blen = (unsigned long)caps[c] - 1;
                    bufs[c][blen] = '\0';
                    db_json_cell(bufs[c], out);
                }
            }
            sb_str(out, "}");
        }
    }
    sb_str(out, "]");
done:
    for (unsigned int c = 0; c < cols; c++) free(bufs[c]);
    free(bufs); free(ob); free(lens); free(nulls); free(caps);
    free(pb); free(il); free(fl);
    if (meta) mysql_free_result(meta);
    mysql_stmt_close(stmt);
    return rc;
}

static int mysql_write_exec(void *conn, const char *sql, const char **params,
                            int nparams, int *affected, char *err, size_t errsz) {
    char body[SQL_MAX];
    const char *verr = db_validate_write(sql, body, sizeof body);
    if (verr) { snprintf(err, errsz, "%s", verr); return 1; }
    MYSQL *m = (MYSQL *)conn;
    MYSQL_STMT *stmt = mysql_stmt_init(m);
    if (!stmt) { snprintf(err, errsz, "mysql: mysql_stmt_init failed"); return 1; }
    int rc = 0;
    MYSQL_BIND *pb = NULL;
    long long *il = NULL;
    double *fl = NULL;
    if (mysql_stmt_prepare(stmt, body, (unsigned long)strlen(body)) != 0) {
        db_err_sanitized(err, errsz, "mysql: prepare failed",
                         mysql_stmt_error(stmt));
        rc = 1; goto done;
    }
    if (nparams > 0) {
        pb = calloc((size_t)nparams, sizeof *pb);
        il = calloc((size_t)nparams, sizeof *il);
        fl = calloc((size_t)nparams, sizeof *fl);
        if (!pb || !il || !fl) {
            snprintf(err, errsz, "mysql: out of memory binding params");
            rc = 1; goto done;
        }
        mysql_fill_bind(pb, il, fl, params, nparams);
        if (mysql_stmt_bind_param(stmt, pb) != 0) {
            db_err_sanitized(err, errsz, "mysql: bind params failed",
                             mysql_stmt_error(stmt));
            rc = 1; goto done;
        }
    }
    if (mysql_stmt_execute(stmt) != 0) {
        db_err_sanitized(err, errsz, "mysql: query failed",
                         mysql_stmt_error(stmt));
        rc = 1; goto done;
    }
    *affected = (int)mysql_stmt_affected_rows(stmt);
done:
    free(pb); free(il); free(fl);
    mysql_stmt_close(stmt);
    return rc;
}

static void mysql_backend_close(const char *dsn, void *conn) {
    mysql_pool_put(dsn, (MYSQL *)conn);
}

#endif /* HAVE_MYSQL */

/* ===================== DSL 分派入口 ===================== */

int db_query_json(const char *dsn, const char *sql, const char **params,
                  int nparams, sbuf *out, char *err, size_t errsz) {
    switch (db_scheme(dsn)) {
    case DB_POSTGRES:
#ifdef HAVE_LIBPQ
    {
        void *conn = NULL;
        if (pg_open(dsn, &conn, err, errsz)) return 1;
        int rc = pg_query_json(conn, sql, params, nparams, out, err, errsz);
        if (rc) { PQfinish((PGconn *)conn); return rc; }  /* 出错不还池 */
        pg_close(dsn, conn);
        return rc;
    }
#else
        snprintf(err, errsz,
                 "postgres:// not compiled in (rebuild with WITH_PG=1)");
        return 1;
#endif
    case DB_MYSQL:
#ifdef HAVE_MYSQL
    {
        void *conn = NULL;
        if (mysql_open(dsn, &conn, err, errsz)) return 1;
        int rc = mysql_query_json(conn, sql, params, nparams, out, err, errsz);
        mysql_backend_close(dsn, conn);
        return rc;
    }
#else
        snprintf(err, errsz,
                 "mysql:// not compiled in (rebuild with WITH_MYSQL=1)");
        return 1;
#endif
    case DB_SQLITE:
    default:
        /* 空 / 普通路径 → SQLite 默认路径(env SQLITE_DB), 与改造前一致 */
        return sqlite_query_json(dsn, sql, params, nparams, out, err, errsz);
    }
}

int db_write_exec(const char *dsn, const char *sql, const char **params,
                  int nparams, int *affected, char *err, size_t errsz) {
    switch (db_scheme(dsn)) {
    case DB_POSTGRES:
#ifdef HAVE_LIBPQ
    {
        void *conn = NULL;
        if (pg_open(dsn, &conn, err, errsz)) return 1;
        int rc = pg_write_exec(conn, sql, params, nparams, affected,
                               err, errsz);
        if (rc) { PQfinish((PGconn *)conn); return rc; }  /* 出错不还池 */
        pg_close(dsn, conn);
        return rc;
    }
#else
        snprintf(err, errsz,
                 "postgres:// not compiled in (rebuild with WITH_PG=1)");
        return 1;
#endif
    case DB_MYSQL:
#ifdef HAVE_MYSQL
    {
        void *conn = NULL;
        if (mysql_open(dsn, &conn, err, errsz)) return 1;
        int rc = mysql_write_exec(conn, sql, params, nparams, affected,
                                  err, errsz);
        mysql_backend_close(dsn, conn);
        return rc;
    }
#else
        snprintf(err, errsz,
                 "mysql:// not compiled in (rebuild with WITH_MYSQL=1)");
        return 1;
#endif
    case DB_SQLITE:
    default:
        return sqlite_write_exec(dsn, sql, params, nparams, affected,
                                 err, errsz);
    }
}
