/* MCP stdio client — see mcp.h. Wire protocol is newline-delimited JSON-RPC
 * 2.0 over the child's stdin/stdout. Responses are matched by their "id";
 * notifications (no/mismatched id) are drained and ignored.

 * JSON handling rides minijson (search-style reads, sbuf writes). The one
 * place a DOM would help — pretty-printing structuredContent — borrows
 * jq(1) from PATH (a subprocess reading the JSON on stdin), so the server
 * binary itself still links only libc. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "internal.h"
#include "minijson.h"
#include "mcp.h"
#include "tools.h"

#define MCP_RESP_MAX (1 << 20)       /* 1MB cap on any single response */
#define MCP_BOOT_TIMEOUT 25          /* initialize + tools/list (seconds) */
#define MCP_CALL_TIMEOUT 900         /* tools/call (seconds, pse-review 最长约 11 分钟) */
#define MCP_STRUC_MAX 8192           /* jq'd structuredContent cap */
#define MCP_TEXT_MAX 512             /* per content[].text item cap */
#define MCP_FINAL_MAX 65536          /* final assembled tool text cap */

/* JSON-RPC initialize params (kept near the top so the resident-connection
 * helpers below can reference it). */
static const char MCP_INIT_PARAMS[] =
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"agent-httpd\",\"version\":\"1.0\"}}";

static McpServerCfg g_servers[MCP_SERVERS_MAX];
static int g_nservers = 0;
static McpToolInfo g_tools[MCP_TOOLS_MAX];
static int g_ntools = 0;

/* forward declaration: defined further down with the tool-catalog helpers */
static void mcp_digest_tools(const McpServerCfg *s, const char *resp);

/* ---- config --------------------------------------------------------- */

/* Profile allow-list: MCP_ALLOW="id1,id2" restricts which configured MCP
 * servers get spawned and registered; unset/empty allows everything. Lets
 * each hosting example expose only the servers its task needs (e.g. invest
 * keeps portfolio-check/pse-review without the echo demo stub). */
static int mcp_allowed(const char *id) {
    const char *allow = getenv("MCP_ALLOW");
    if (!allow || !allow[0]) return 1;
    char buf[1024];
    set_str(buf, sizeof buf, allow);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        trim_whitespace(tok);
        if (strcmp(tok, id) == 0) return 1;
    }
    return 0;
}

/* Copy the next {...} from a JSON array (string/quote aware) into buf.
 * *pp advances past the object. Returns 1 on success, 0 at end/malformed. */
static int cfg_next_obj(const char **pp, char *buf, size_t bufsz) {
    const char *p = jws(*pp);
    if (*p != '{') return 0;
    const char *start = p;
    int depth = 0, in_str = 0;
    for (const char *q = p; *q; q++) {
        if (in_str) {
            if (*q == '\\' && q[1]) q++;
            else if (*q == '"') in_str = 0;
            continue;
        }
        if (*q == '"') in_str = 1;
        else if (*q == '{') depth++;
        else if (*q == '}' && --depth == 0) {
            size_t n = (size_t)(q - start) + 1;
            if (n >= bufsz) return 0;
            memcpy(buf, start, n);
            buf[n] = '\0';
            *pp = q + 1;
            return 1;
        }
    }
    return 0;
}

/* Split a whitespace-delimited args string into server->argv. */
static int cfg_split_args(McpServerCfg *s, const char *command,
                          const char *args) {
    char tmp[MCP_ARGS_MAX * MAX_PATH_SIZE];
    set_str(tmp, sizeof tmp, args ? args : "");
    s->argc = 0;
    set_str(s->argv[0], MAX_PATH_SIZE, command);
    s->argc = 1;
    if (s->argc >= MCP_ARGS_MAX) return -1;
    for (char *tok = strtok(tmp, " \t"); tok && s->argc < MCP_ARGS_MAX;
         tok = strtok(NULL, " \t")) {
        set_str(s->argv[s->argc], MAX_PATH_SIZE, tok);
        s->argc++;
    }
    return 0;
}

static void cfg_add_server(const char *obj) {
    if (g_nservers >= MCP_SERVERS_MAX) return;
    const char *tv = jfind_value(obj, "transport");
    if (tv && *tv == '"') {
        char t[16];
        const char *tp = tv;
        if (jread_string(&tp, t, sizeof t) && strcmp(t, "stdio") != 0) {
            fprintf(stderr, "[mcp] skip server: only stdio transport today\n");
            return;
        }
    }
    const char *cv = jfind_value(obj, "command");
    const char *av = jfind_value(obj, "args");
    const char *iv = jfind_value(obj, "id");
    if (!cv || *cv != '"') return;
    McpServerCfg s;
    memset(&s, 0, sizeof s);
    const char *cp = cv;
    char command[MAX_PATH_SIZE];
    if (!jread_string(&cp, command, sizeof command)) return;
    char args[MAX_PATH_SIZE] = "";
    if (av && *av == '"') {
        const char *ap = av;
        if (!jread_string(&ap, args, sizeof args)) return;
    }
    if (cfg_split_args(&s, command, args) != 0) return;
    if (!iv || *iv != '"') {
        set_str(s.id, sizeof s.id, s.argv[0]);
    } else {
        const char *ip = iv;
        if (!jread_string(&ip, s.id, sizeof s.id)) return;
    }
    const char *ap = jfind_value(obj, "approval");
    if (ap) {
        if (*ap == 't') s.approval = 1;
        else if (*ap == 'f') s.approval = 0;
    }
    if (!mcp_allowed(s.id)) return; /* profile allow-list: never spawn/register */
    for (int i = 0; i < g_nservers; i++) {
        if (strcmp(g_servers[i].id, s.id) == 0) return;
    }
    g_servers[g_nservers++] = s;
    fprintf(stderr, "[mcp] server %s: %s\n", s.id, s.argv[0]);
}

static void cfg_from_text(const char *text) {
    if (!text) return;
    const char *p = text;
    if (*p == '[') p++;
    for (;;) {
        char obj[4096];
        p = jws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '{') break;
        if (!cfg_next_obj(&p, obj, sizeof obj)) break;
        cfg_add_server(obj);
    }
}

static void mcp_config_load(void) {
    /* file first (persistent/UI-managed), then env override merges in,
     * then the router-synced file (written by router_sync_all pre-fork) */
    static const char *const files[] = {".data/mcp-servers.json",
                                        ".data/mcp-servers-router.json"};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        FILE *f = fopen(files[i], "r");
        if (f) {
            sbuf b = {0};
            char chunk[4096];
            size_t r;
            while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
                sb_mem(&b, chunk, r);
                if (b.oom) break;
            }
            fclose(f);
            cfg_from_text(b.p);
            free(b.p);
        }
    }
    cfg_from_text(getenv("MCP_SERVERS"));
}

/* ---- child spawn + newline JSON-RPC exchange ------------------------- */

typedef struct {
    pid_t pid;
    int to_child[2];   /* [0] read end unused, [1] write end */
    int from_child[2]; /* [0] read end, [1] write end unused */
} McpProc;

static void mcpproc_close(McpProc *p) {
    if (p->to_child[1] >= 0) close(p->to_child[1]);
    if (p->from_child[0] >= 0) close(p->from_child[0]);
    p->to_child[1] = -1;
    p->from_child[0] = -1;
}

static int mcpproc_spawn(const McpServerCfg *s, McpProc *p) {
    int in[2], out[2];
    if (pipe(in) < 0 || pipe(out) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        char *argv[MCP_ARGS_MAX + 1];
        for (int i = 0; i < s->argc; i++) argv[i] = strdup(s->argv[i]);
        argv[s->argc] = NULL;
        /* H1: third-party MCP servers are fetched/run via npx; they must not
         * inherit our LLM credentials. Keep MCP_FS_ROOT — the filesystem
         * server needs it as its sandbox root. (The CGI path scrubs the same
         * trio in cgi.c.) */
        unsetenv("LLM_API_KEY");
        unsetenv("LLM_API_URL");
        unsetenv("LLM_MODEL");
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    p->pid = pid;
    p->to_child[0] = -1;
    p->to_child[1] = in[1];
    p->from_child[0] = out[0];
    p->from_child[1] = -1;
    return 0;
}

static void sbuf_copy(sbuf *dst, const sbuf *src) {
    if (src->p && src->len) sb_mem(dst, src->p, src->len);
}

/* Whether the accumulated payload contains the response for `id`
 * ("id": 1 vs "id":1 — MCP peers are not whitespace-strict). */
static int carries_response(const char *p, long id) {
    const char *s = p;
    while ((s = strstr(s, "\"id\"")) != NULL) {
        s += 4;
        while (*s == ' ' || *s == '\t') s++;
        if (*s != ':') continue;
        s++;
        while (*s == ' ' || *s == '\t') s++;
        char *end = NULL;
        long v = strtol(s, &end, 10);
        if (end != s && v == id) return 1;
    }
    return 0;
}

/* Read until the response whose accumulated payload contains `"id":<id>`
 * lands (newline-delimited JSON: the whole response arrives in one or two
 * reads; the accumulator is capped). Returns 1 got it, 0 on EOF/timeout/
 * overflow; the response JSON text is copied into out. */
static int mcpproc_read_response(McpProc *p, long id, int timeout_s,
                                 sbuf *out) {
    time_t deadline = time(NULL) + timeout_s;
    sbuf acc = {0};

    int rc = 0;
    for (;;) {
        long remain = deadline - time(NULL);
        if (remain <= 0) break;
        struct pollfd pfd = { p->from_child[0], POLLIN, 0 };
        int pr = poll(&pfd, 1, (int)(remain * 1000));
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) break; /* timeout */
        char chunk[8192];
        ssize_t n = read(p->from_child[0], chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break; /* EOF */
        sb_mem(&acc, chunk, (size_t)n);
        if (acc.oom || acc.len > MCP_RESP_MAX) break;
        if (carries_response(acc.p, id)) {
            rc = 1;
            break;
        }
    }
    if (rc && out && acc.p) {
        sbuf_copy(out, &acc);
    }
    free(acc.p);
    return rc;
}

/* Send one JSON-RPC request (or notification when params points at a
 * pre-built full object and id<0 is given). Returns 0 on write success. */
static int mcpproc_send(McpProc *p, long id, const char *method,
                        const char *params_json, int notification) {
    sbuf line = {0};
    if (notification) {
        sb_str(&line, "{\"jsonrpc\":\"2.0\",\"method\":");
        sb_json_str(&line, method);
        sb_str(&line, "}");
    } else {
        sb_str(&line, "{\"jsonrpc\":\"2.0\",\"id\":");
        char idb[32];
        snprintf(idb, sizeof idb, "%ld", id);
        sb_str(&line, idb);
        sb_str(&line, ",\"method\":");
        sb_json_str(&line, method);
        if (params_json && params_json[0]) {
            sb_str(&line, ",\"params\":");
            sb_str(&line, params_json);
        }
        sb_str(&line, "}");
    }
    sb_chr(&line, '\n');
    int rc = -1;
    if (!line.oom) {
        /* short bodies stay within PIPE_BUF so a single write is atomic;
         * still surface a failed write instead of claiming success */
        ssize_t w = write(p->to_child[1], line.p, line.len);
        rc = (w == (ssize_t)line.len) ? 0 : -1;
    }
    free(line.p);
    return rc;
}

/* ---- resident connection pool --------------------------------------- */
/* A spawned MCP server is kept alive across calls: each worker process
 * hands its own pipe pair + pid, so every tools/call reuses the handshake
 * instead of paying fork()+exec()+initialize+tools/list again. The pool is
 * parallel to g_servers; the worker model is prefork + serial-per-connection
 * (see worker.c), so a module-level pool needs no locking. */

typedef struct {
    McpProc proc;   /* pid + pipe pair (reused for every call) */
    int alive;      /* 1 once spawned + handshaked, 0 if dead/empty */
    long seq;       /* next JSON-RPC request id to issue */
} McpConn;

static McpConn g_conn[MCP_SERVERS_MAX];

static void mcp_conn_kill(McpConn *c) {
    mcpproc_close(&c->proc);
    if (c->proc.pid > 0) {
        kill(c->proc.pid, SIGKILL);
        for (int i = 0; i < 200; i++) {
            pid_t r = waitpid(c->proc.pid, NULL, WNOHANG);
            if (r == c->proc.pid || (r < 0 && errno == ECHILD)) break;
            usleep(10 * 1000);
        }
        c->proc.pid = -1;
    }
    c->alive = 0;
}

/* Kill every resident child and reset the pool (called on (re)load so a
 * SIGHUP resync or a fresh config never inherits a stale child). */
static void mcp_conn_reset_all(void) {
    for (int i = 0; i < MCP_SERVERS_MAX; i++) {
        if (g_conn[i].alive) mcp_conn_kill(&g_conn[i]);
        g_conn[i].proc.pid = -1;
        g_conn[i].proc.to_child[1] = -1;
        g_conn[i].proc.from_child[0] = -1;
        g_conn[i].alive = 0;
        g_conn[i].seq = 3; /* init uses 1, tools/list uses 2 */
    }
}

/* True when the resident child has exited (zombie reaped, or no longer
 * killable). A surviving child passes kill(pid,0). */
static int mcp_conn_dead(McpConn *c) {
    if (c->proc.pid <= 0) return 1;
    pid_t r = waitpid(c->proc.pid, NULL, WNOHANG);
    if (r == c->proc.pid || (r < 0 && errno == ECHILD)) return 1;
    if (kill(c->proc.pid, 0) != 0) return 1;
    return 0;
}

/* Discard any bytes already buffered by the child (e.g. unsolicited
 * notifications) so the next read_response starts clean. Non-blocking:
 * returns once nothing is immediately readable. */
static void mcp_drain_stale(McpProc *p) {
    int fd = p->from_child[0];
    if (fd < 0) return;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
    }
    fcntl(fd, F_SETFL, fl);
}

/* Spawn + handshake a server, harvesting its tool catalog; on success the
 * child is left resident in *c (alive=1). On failure the child is killed. */
static int mcp_conn_start(const McpServerCfg *s, McpConn *c) {
    memset(&c->proc, 0, sizeof c->proc);
    c->proc.pid = -1;
    c->proc.to_child[1] = -1;
    c->proc.from_child[0] = -1;
    if (mcpproc_spawn(s, &c->proc) < 0) return -1;
    int rc = -1;
    sbuf init_resp = {0};
    if (mcpproc_send(&c->proc, 1, "initialize", MCP_INIT_PARAMS, 0) == 0 &&
        mcpproc_read_response(&c->proc, 1, MCP_BOOT_TIMEOUT, &init_resp)) {
        mcpproc_send(&c->proc, -1, "notifications/initialized", NULL, 1);
        sbuf list_resp = {0};
        if (mcpproc_send(&c->proc, 2, "tools/list", NULL, 0) == 0 &&
            mcpproc_read_response(&c->proc, 2, MCP_BOOT_TIMEOUT, &list_resp)) {
            mcp_digest_tools(s, list_resp.p);
            rc = 0;
        }
        free(list_resp.p);
    }
    free(init_resp.p);
    if (rc == 0) {
        c->alive = 1;
        c->seq = 3;
    } else {
        mcp_conn_kill(c);
    }
    return rc;
}

/* ---- resident tools/call -------------------------------------------- */

/* Execute tools/call against a resident server connection (spawned once in
 * mcp_init, reused for every call). A crashed/dead child is respawned
 * lazily; a child that wedges mid-call is killed and reported as a failure
 * (the caller retries on the next turn). */
static int mcp_tools_call_raw(const McpServerCfg *s, const char *tool,
                              const char *args_json, sbuf *resp) {
    int idx = (int)(s - g_servers);
    if (idx < 0 || idx >= g_nservers) return -1;
    McpConn *c = &g_conn[idx];
    if (!c->alive || mcp_conn_dead(c)) {
        if (mcp_conn_start(s, c) != 0) return -1;
    }

    mcp_drain_stale(&c->proc); /* clear stray notifications from a prior call */

    sbuf params = {0};
    sb_str(&params, "{\"name\":");
    sb_json_str(&params, tool);
    sb_str(&params, ",\"arguments\":");
    sb_mem(&params, args_json ? args_json : "{}", args_json ? strlen(args_json) : 2);
    sb_str(&params, "}");
    sbuf call_resp = {0};
    int rc = -1;
    long id = c->seq++;
    if (mcpproc_send(&c->proc, id, "tools/call", params.p, 0) == 0 &&
        mcpproc_read_response(&c->proc, id, MCP_CALL_TIMEOUT, &call_resp)) {
        sbuf_copy(resp, &call_resp);
        rc = 0;
    } else {
        /* child died or wedged during the call: drop it so the next call
         * respawns a fresh one. */
        mcp_conn_kill(c);
    }
    free(call_resp.p);
    free(params.p);
    return rc;
}

/* ---- result formatting ---------------------------------------------- */

/* Pretty-print a JSON document via jq(1) (borrowed): subprocess reads
 * stdin, writes indented JSON to stdout. Empty output when jq is absent. */
static void jq_pretty_print(const char *json, sbuf *out) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return;
    }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execlp("jq", "jq", ".", (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    size_t off = 0, len = strlen(json);
    while (off < len) {
        ssize_t w = write(in_pipe[1], json + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
    close(in_pipe[1]);
    size_t n = 0;
    char chunk[4096];
    for (;;) {
        ssize_t r = read(out_pipe[0], chunk, sizeof chunk);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        if (n + (size_t)r > MCP_STRUC_MAX) {
            sb_mem(out, chunk, MCP_STRUC_MAX - n > (size_t)r ? (size_t)r : MCP_STRUC_MAX - n);
            break;
        }
        sb_mem(out, chunk, (size_t)r);
        n += (size_t)r;
    }
    close(out_pipe[0]);
    kill(pid, SIGKILL);
    {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

/* MCP servers pipe stdout and stderr into the same fd; some (e.g. the
 * filesystem server) print a startup banner before their first JSON reply.
 * Locate the JSON object so the tolerant reader's top-level expectations
 * (must open with '{') hold. */
static const char *json_body(const char *buf) {
    if (!buf) return NULL;
    return strchr(buf, '{');
}

/* Extract readable text from an MCP tools/call response (result JSON text):
 *  - error                    -> "error: <message>"
 *  - result.content[].text    -> joined with newlines (readable view wins)
 *  - result.structuredContent -> pretty-printed via jq
 *  - result.text              -> flat string
 *  - else                     -> raw result JSON */
static void mcp_format_result(const char *resp, sbuf *out) {
    resp = json_body(resp); /* tolerate server startup noise on the pipe */
    if (!resp) {
        sb_str(out, "error: no JSON in mcp response");
        return;
    }
    const char *err = jfind_value(resp, "error");
    if (err && *err == '{') {
        const char *em = jfind_value(err, "message");
        sb_str(out, "error: ");
        if (em && *em == '"') {
            char msg[256];
            const char *p = em;
            if (jread_string(&p, msg, sizeof msg)) sb_str(out, msg);
            else sb_str(out, "mcp tool failed");
        } else {
            sb_str(out, "mcp tool failed");
        }
        return;
    }
    const char *res = jfind_value(resp, "result");
    if (!res || *res != '{') {
        sb_str(out, "error: no result field in mcp response");
        return;
    }

    int emitted = 0;
    const char *content = jfind_value(res, "content");
    if (content && *content == '[') {
        const char *q = jws(content + 1);
        int first = 1;
        while (*q == '{' && !emitted) {
            /* collect item text */
            char item[8192];
            int depth = 0, in_str = 0;
            size_t n = 0;
            for (const char *r = q; *r && n < sizeof item - 1; r++) {
                if (in_str) {
                    if (*r == '\\' && r[1]) { item[n++] = *r; item[n++] = *++r; continue; }
                    if (*r == '"') in_str = 0;
                } else if (*r == '"') in_str = 1;
                else if (*r == '{') depth++;
                else if (*r == '}' && --depth == 0) {
                    item[n++] = *r;
                    item[n] = '\0';
                    q = r + 1;
                    goto have_item;
                }
                item[n++] = *r;
            }
            break; /* malformed */
have_item:;
            const char *tv = jfind_value(item, "text");
            if (tv && *tv == '"') {
                char text[MCP_TEXT_MAX];
                const char *tp = tv;
                if (jread_string(&tp, text, sizeof text)) {
                    if (!first) sb_chr(out, '\n');
                    sb_str(out, text);
                    first = 0;
                    emitted = 1;
                }
            }
            /* isError flag on the item */
            q = jws(q);
            if (*q == ',') q = jws(q + 1);
            else break;
        }
    }
    if (emitted) return;

    const char *sc = jfind_value(res, "structuredContent");
    if (sc && *sc == '{') {
        char copy[MCP_STRUC_MAX + 1];
        set_str(copy, sizeof copy, sc);
        jq_pretty_print(copy, out);
        if (out->len > 0 && out->p) {
            size_t l = out->len;
            while (l > 0 && (out->p[l - 1] == '\n')) out->p[--l] = '\0';
            out->len = l;
            return;
        }
        sb_str(out, copy);
        return;
    }
    const char *flat = jfind_value(res, "text");
    if (flat && *flat == '"') {
        char text[MCP_TEXT_MAX * 4];
        const char *fp = flat;
        if (jread_string(&fp, text, sizeof text)) {
            sb_str(out, text);
            return;
        }
    }
    sb_mem(out, res, strnlen(res, MCP_FINAL_MAX));
}

/* ---- registration + dispatch ---------------------------------------- */

static McpServerCfg *find_server(const char *id) {
    for (int i = 0; i < g_nservers; i++) {
        if (strcmp(g_servers[i].id, id) == 0) return &g_servers[i];
    }
    return NULL;
}

static void mcp_tool_runner(void *data, const char *args_json,
                            const char *session_id, sbuf *result) {
    (void)session_id;
    size_t idx = (size_t)(uintptr_t)data;
    if (idx >= (size_t)g_ntools) {
        sb_str(result, "error: internal mcp tool index out of range");
        return;
    }
    mcp_call(g_tools[idx].mcp_name, args_json, result);
}

void mcp_call(const char *mcp_name, const char *args_json, sbuf *result) {
    const McpToolInfo *t = NULL;
    for (int i = 0; i < g_ntools; i++) {
        if (strcmp(g_tools[i].mcp_name, mcp_name) == 0) t = &g_tools[i];
    }
    if (!t) {
        sb_str(result, "error: unknown mcp tool '");
        sb_str(result, mcp_name);
        sb_str(result, "'");
        return;
    }
    McpServerCfg *s = find_server(t->server_id);
    if (!s) {
        sb_str(result, "error: mcp server '");
        sb_str(result, t->server_id);
        sb_str(result, "' not configured");
        return;
    }
    fprintf(stderr, "[mcp] call %s (approval=%d)\n", mcp_name, s->approval);
    sbuf resp = {0};
    if (mcp_tools_call_raw(s, t->tool_name, args_json, &resp) != 0) {
        sb_str(result, "error: mcp server '");
        sb_str(result, s->id);
        sb_str(result, "' failed (spawn error or timeout)");
        free(resp.p);
        return;
    }
    mcp_format_result(resp.p, result);
    free(resp.p);
    if (result->len > MCP_FINAL_MAX) result->len = MCP_FINAL_MAX;
}

/* Copy exactly the extent of one JSON value (values embedded in a larger
 * document have no NUL at their boundary — memcpy the measured span). */
static void copy_json_value(const char *v, char *out, size_t outsz) {
    const char *p = v;
    if (v && jskip_value(&p) == 0 && p > v) {
        size_t n = (size_t)(p - v);
        if (n > outsz - 1) {
            /* Byte-truncating a JSON value leaves invalid JSON behind, and
             * this lands in the tools schema sent upstream: one poisoned
             * entry makes every agent request 400. Fall back to a valid
             * empty object instead of a truncated fragment. */
            fprintf(stderr, "[mcp] tool inputSchema.properties too long "
                            "(%zu > %zu), using {}\n", n, outsz - 1);
            n = 2;
            v = "{}";
        }
        memcpy(out, v, n);
        out[n] = '\0';
    } else if (outsz) {
        out[0] = '\0';
    }
}

static void mcp_digest_tools(const McpServerCfg *s, const char *resp) {
    if (s && !mcp_allowed(s->id)) return; /* belt-and-braces */
    resp = json_body(resp); /* tolerate server startup noise on the pipe */
    const char *res = resp ? jfind_value(resp, "result") : NULL;
    if (!res || *res != '{') {
        fprintf(stderr, "[mcp] %s: tools/list failed\n", s->id);
        return;
    }
    const char *tools = jfind_value(res, "tools");
    if (!tools || *tools != '[') {
        fprintf(stderr, "[mcp] %s: no tools array\n", s->id);
        return;
    }
    const char *q = jws(tools + 1);
    while (*q == '{' && g_ntools < MCP_TOOLS_MAX) {
        char item[8192];
        int depth = 0, in_str = 0;
        size_t n = 0;
        for (const char *r = q; *r && n < sizeof item - 1; r++) {
            if (in_str) {
                if (*r == '\\' && r[1]) { item[n++] = *r; item[n++] = *++r; continue; }
                if (*r == '"') in_str = 0;
            } else if (*r == '"') in_str = 1;
            else if (*r == '{') depth++;
            else if (*r == '}' && --depth == 0) {
                item[n++] = *r;
                item[n] = '\0';
                q = r + 1;
                goto have_tool;
            }
            item[n++] = *r;
        }
        break;
have_tool:;
        const char *nv = jfind_value(item, "name");
        const char *dv = jfind_value(item, "description");
        const char *iv = jfind_value(item, "inputSchema");
        char name[48] = "", desc[TOOL_DESC_MAX + 1] = "";
        const char *p = nv;
        if (nv && *nv == '"') {
            if (!jread_string(&p, name, sizeof name)) return;
        }
        p = dv;
        if (dv && *dv == '"') jread_string(&p, desc, sizeof desc);
        // Tool name surfaces in the OpenAI tools schema, whose grammar allows
        // only [A-Za-z0-9_-]; a ":" separator (serverId:toolName) gets the whole
        // tool rejected upstream. Use "__" and rely on server_id/tool_name for
        // the real MCP addressing in mcp_call.
        char mcp_name[TOOL_NAME_MAX + 1];
        snprintf(mcp_name, sizeof mcp_name, "%s__%s", s->id, name);
        /* Idempotent resync: skip tools already harvested so repeated SIGHUP
         * re-probes don't grow the McpToolInfo table (registry entries keep
         * their original data index). */
        int found = 0;
        for (int k = 0; k < g_ntools; k++) {
            if (strcmp(g_tools[k].mcp_name, mcp_name) == 0) { found = 1; break; }
        }
        if (found) {
            q = jws(q);
            if (*q == ',') q = jws(q + 1);
            else break;
            continue;
        }
        McpToolInfo *t = &g_tools[g_ntools];
        set_str(t->mcp_name, sizeof t->mcp_name, mcp_name);
        set_str(t->server_id, sizeof t->server_id, s->id);
        set_str_utf8(t->tool_name, sizeof t->tool_name, name);
        set_str_utf8(t->desc, sizeof t->desc, desc[0] ? desc : t->mcp_name);
        /* keep inputSchema.properties, not the whole schema */
        if (iv && *iv == '{') {
            const char *props = jfind_value(iv, "properties");
            if (props && *props == '{') {
                copy_json_value(props, t->props, sizeof t->props);
            } else {
                set_str(t->props, sizeof t->props, "{}");
            }
        } else {
            set_str(t->props, sizeof t->props, "{}");
        }
        tools_register(t->mcp_name, t->desc, t->props, mcp_tool_runner,
                       (void *)(uintptr_t)g_ntools);
        fprintf(stderr, "[mcp] tool: %s\n", t->mcp_name);
        g_ntools++;
        q = jws(q);
        if (*q == ',') q = jws(q + 1);
        else break;
    }
}

int mcp_init(void) {
    /* MCP_FS_ROOT is the filesystem MCP server's sandbox root: everything
     * inside it is readable (and writable) by the model through chat
     * tools. Warn when that root sweeps in credential or state material so
     * misconfiguration is visible at startup, not after a chat reads it. */
    const char *fs_root = getenv("MCP_FS_ROOT");
    if (fs_root && fs_root[0]) {
        char probe[MAX_PATH_SIZE];
        snprintf(probe, sizeof probe, "%s/.env", fs_root);
        if (access(probe, F_OK) == 0) {
            fprintf(stderr,
                    "mcp: MCP_FS_ROOT '%s' contains a .env file - the "
                    "filesystem MCP server can read it via chat tools. "
                    "Point MCP_FS_ROOT at the narrowest directory you "
                    "tolerate exposing to the model.\n", fs_root);
        }
        snprintf(probe, sizeof probe, "%s/.data", fs_root);
        if (access(probe, F_OK) == 0) {
            fprintf(stderr,
                    "mcp: MCP_FS_ROOT '%s' contains the .data session "
                    "store - the filesystem MCP server can read every "
                    "saved transcript via chat tools. Narrow MCP_FS_ROOT "
                    "if that is not intended.\n", fs_root);
        }
    }

    /* SIGHUP resync re-reads the authoritative config files, so the server
     * table is rebuilt from scratch each time (children spawned before the
     * reload keep their own COW copy and stay consistent). */
    g_nservers = 0;
    memset(g_servers, 0, sizeof g_servers);
    mcp_conn_reset_all(); /* kill any resident children from an old config */
    mcp_config_load();
    if (g_nservers == 0) return 0;
    /* Whole-init wall clock: npx-backed servers cold-start in seconds each
     * and the loop is serial, so N servers x 25s handshake timeout can park
     * the boot for minutes (Docker healthchecks read that as a failed
     * start). Tools are sugar — past the budget the remaining servers are
     * skipped; a later SIGHUP resync re-registers them. 0 = unlimited. */
    const char *bs = getenv("MCP_INIT_BUDGET_SECONDS");
    int budget = bs ? atoi(bs) : 30;
    time_t deadline = (budget > 0) ? time(NULL) + budget : 0;
    for (int i = 0; i < g_nservers; i++) {
        if (deadline && time(NULL) >= deadline) {
            fprintf(stderr, "[mcp] init budget exhausted, skipping %d server(s)\n",
                    g_nservers - i);
            break;
        }
        McpServerCfg *s = &g_servers[i];
        /* Per-phase handshake timeout rides the remaining budget (min 1s,
         * max the usual 25s) so one slow npx cold-start can never spend
         * more than what's left of the wall. */
        int per_to = 25;
        if (deadline) {
            long left = (long)(deadline - time(NULL));
            per_to = (left < 1) ? 1 : (left > 25 ? 25 : (int)left);
        }
        McpConn *c = &g_conn[i];
        memset(&c->proc, 0, sizeof c->proc);
        c->proc.pid = -1;
        c->proc.to_child[1] = -1;
        c->proc.from_child[0] = -1;
        c->alive = 0;
        if (mcpproc_spawn(s, &c->proc) < 0) {
            fprintf(stderr, "[mcp] %s: spawn failed\n", s->id);
            continue;
        }
        int rc = -1;
        sbuf init_resp = {0};
        if (mcpproc_send(&c->proc, 1, "initialize", MCP_INIT_PARAMS, 0) == 0 &&
            mcpproc_read_response(&c->proc, 1, per_to, &init_resp)) {
            mcpproc_send(&c->proc, -1, "notifications/initialized", NULL, 1);
            sbuf list_resp = {0};
            if (mcpproc_send(&c->proc, 2, "tools/list", NULL, 0) == 0 &&
                mcpproc_read_response(&c->proc, 2, per_to, &list_resp)) {
                mcp_digest_tools(s, list_resp.p);
                rc = 0;
            }
            free(list_resp.p);
        }
        free(init_resp.p);
        if (rc == 0) {
            /* Keep the child resident: every later tools/call reuses this
             * pipe pair instead of paying fork()+exec()+handshake again. */
            c->alive = 1;
            c->seq = 3;
            fprintf(stderr, "[mcp] %s: resident (pid %d)\n", s->id,
                    (int)c->proc.pid);
        } else {
            mcp_conn_kill(c);
            fprintf(stderr, "[mcp] %s: handshake failed\n", s->id);
        }
    }
    return g_ntools;
}

int mcp_server_count(void) {
    return g_nservers;
}

const McpServerCfg *mcp_server(int i) {
    if (i < 0 || i >= g_nservers) return NULL;
    return &g_servers[i];
}

int mcp_tool_count(void) {
    return g_ntools;
}

const McpToolInfo *mcp_tool(int i) {
    if (i < 0 || i >= g_ntools) return NULL;
    return &g_tools[i];
}