/* Agent built-in + dynamic tools — see tools.h. Everything here is fork+exec
 * or pure computation: no new library dependencies, tool results stay plain
 * text (the agent loop JSON-escapes them into role:"tool" messages).
 *
 * Sandbox notes (teaching server, fails closed where cheap):
 *   - read_file resolves through resolve_within() against the web root;
 *   - fetch_url only allows http/https (curl --proto) and caps the body;
 *   - skill-run only touches skills indexed at startup (never a raw path);
 *   - remember/recall write to the per-session memory pool (.data/...),
 *     key validated against metacharacters before it touches disk. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "internal.h"
#include "minijson.h"
#include "tools.h"
#include "skills.h"
#include "session.h"

#define TOOL_READ_MAX 8192
#define TOOL_FETCH_MAX 16384
#define SKILL_BODY_MAX 16384

/* ---- registry ------------------------------------------------------ */

static ToolDef g_tools[TOOL_MAX];
static int g_ntools = 0;
static char g_schema_cache[TOOL_MAX * (TOOL_NAME_MAX + TOOL_DESC_MAX +
                                      TOOL_PARAMS_MAX + 256) + 256];
static int g_schema_dirty = 1;

static const char *tool_params_or_empty(const ToolDef *t) {
    return t->params[0] ? t->params : "{}";
}

const char *tools_schema_json(void) {
    if (!g_schema_dirty) return g_schema_cache;
    sbuf b = {0};
    sb_chr(&b, '[');
    for (int i = 0; i < g_ntools; i++) {
        if (i) sb_chr(&b, ',');
        sb_str(&b, "{\"type\":\"function\",\"function\":{\"name\":");
        sb_json_str(&b, g_tools[i].name);
        sb_str(&b, ",\"description\":");
        sb_json_str(&b, g_tools[i].desc);
        sb_str(&b, ",\"parameters\":{\"type\":\"object\",\"properties\":");
        sb_str(&b, tool_params_or_empty(&g_tools[i]));
        sb_str(&b, ",\"required\":[]}}}");
    }
    sb_chr(&b, ']');
    if (b.oom) {
        free(b.p);
        return "[]";
    }
    set_str(g_schema_cache, sizeof g_schema_cache, b.p ? b.p : "[]");
    if (b.len + 1 > sizeof g_schema_cache) {
        fprintf(stderr,
                "[tools] schema cache overflow (%zu bytes needed, %zu capped) — "
                "request body will be truncated UTF-8\n",
                b.len, sizeof g_schema_cache);
    }
    free(b.p);
    g_schema_dirty = 0;
    return g_schema_cache;
}

int tools_register(const char *name, const char *desc, const char *params_json,
                   ToolFn fn, void *data) {
    if (!name || !fn || g_ntools >= TOOL_MAX) return -1;
    for (int i = 0; i < g_ntools; i++) {
        if (strcmp(g_tools[i].name, name) == 0) return -1;
    }
    ToolDef *t = &g_tools[g_ntools];
    size_t nb = utf8_valid_prefix(name, TOOL_NAME_MAX);
    memcpy(t->name, name, nb);
    t->name[nb] = '\0';
    nb = utf8_valid_prefix(desc ? desc : "", TOOL_DESC_MAX);
    memcpy(t->desc, desc ? desc : "", nb);
    t->desc[nb] = '\0';
    nb = utf8_valid_prefix(params_json ? params_json : "{}", TOOL_PARAMS_MAX);
    memcpy(t->params, params_json ? params_json : "{}", nb);
    t->params[nb] = '\0';
    t->fn = fn;
    t->data = data;
    g_ntools++;
    g_schema_dirty = 1;
    return 0;
}

int tools_count(void) {
    return g_ntools;
}

const ToolDef *tools_get(int i) {
    if (i < 0 || i >= g_ntools) return NULL;
    return &g_tools[i];
}

/* ---- arg helpers --------------------------------------------------- */

/* Extract a string argument from the raw tool-call arguments text. */
static int tool_str_arg(const char *args, const char *key, char *out,
                        size_t outsz) {
    const char *v = jfind_value(args ? args : "", key);
    if (!v || *v != '"') return -1;
    return jread_string(&v, out, outsz) ? 0 : -1;
}

/* ---- calc: recursive-descent arithmetic ---------------------------- */

typedef struct {
    const char *s;
    int err;
} CalcP;

static int calc_ws(CalcP *c) {
    while (*c->s == ' ' || *c->s == '\t') c->s++;
    return 0;
}

static double calc_expr(CalcP *c);

static double calc_primary(CalcP *c) {
    calc_ws(c);
    if (*c->s == '(') {
        c->s++;
        double v = calc_expr(c);
        calc_ws(c);
        if (*c->s != ')') {
            c->err = 1;
            return 0;
        }
        c->s++;
        return v;
    }
    if (isdigit((unsigned char)*c->s) || *c->s == '.') {
        char *end = NULL;
        double v = strtod(c->s, &end);
        if (end == c->s) {
            c->err = 1;
            return 0;
        }
        c->s = end;
        return v;
    }
    c->err = 1;
    return 0;
}

static double calc_power(CalcP *c) {
    double base = calc_primary(c);
    calc_ws(c);
    if (*c->s == '^') { /* right-associative */
        c->s++;
        double expv = calc_power(c);
        return pow(base, expv);
    }
    return base;
}

static double calc_unary(CalcP *c) {
    calc_ws(c);
    if (*c->s == '-') {
        c->s++;
        return -calc_unary(c);
    }
    if (*c->s == '+') {
        c->s++;
        return calc_unary(c);
    }
    return calc_power(c);
}

static double calc_term(CalcP *c) {
    double v = calc_unary(c);
    for (;;) {
        calc_ws(c);
        if (*c->s == '*') {
            c->s++;
            v *= calc_unary(c);
        } else if (*c->s == '/') {
            c->s++;
            double d = calc_unary(c);
            if (d == 0) {
                c->err = 1;
                return 0;
            }
            v /= d;
        } else if (*c->s == '%') {
            c->s++;
            double d = calc_unary(c);
            if (d == 0) {
                c->err = 1;
                return 0;
            }
            v = fmod(v, d);
        } else {
            return v;
        }
    }
}

static double calc_expr(CalcP *c) {
    double v = calc_term(c);
    for (;;) {
        calc_ws(c);
        if (*c->s == '+') {
            c->s++;
            v += calc_term(c);
        } else if (*c->s == '-') {
            c->s++;
            v -= calc_term(c);
        } else {
            return v;
        }
    }
}

static void tool_calc(void *data, const char *args,
                          const char *session_id, sbuf *result) {
    (void)data;
    (void)session_id;
    char expr[512];
    if (tool_str_arg(args, "expression", expr, sizeof expr) != 0) {
        sb_str(result, "error: missing string argument 'expression'");
        return;
    }
    CalcP c = { expr, 0 };
    double v = calc_expr(&c);
    calc_ws(&c);
    if (c.err || *c.s != '\0') {
        sb_str(result, "error: cannot parse expression '");
        sb_str(result, expr);
        sb_str(result, "'");
        return;
    }
    if (!(v == v) || v > 1e308 || v < -1e308) { /* NaN / overflow */
        sb_str(result, "error: result out of range");
        return;
    }
    char out[64];
    snprintf(out, sizeof out, "%.12g", v);
    sb_str(result, out);
}

/* ---- get_time ------------------------------------------------------- */

static void tool_get_time(void *data, const char *args,
                          const char *session_id, sbuf *result) {
    (void)data;
    (void)args;
    (void)session_id;
    char human[64], http[40];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(human, sizeof human, "%Y-%m-%d %H:%M:%S %Z", &tmv);
    format_http_date(http, sizeof http, now);
    sb_str(result, "server local time: ");
    sb_str(result, human);
    sb_str(result, " (HTTP date: ");
    sb_str(result, http);
    sb_str(result, ")");
}

/* ---- read_file (web-root sandboxed) --------------------------------- */

static void tool_read_file(void *data, const char *args,
                          const char *session_id, sbuf *result) {
    (void)data;
    (void)session_id;
    char path[512];
    if (tool_str_arg(args, "path", path, sizeof path) != 0) {
        sb_str(result, "error: missing string argument 'path'");
        return;
    }
    char path_with_slash[512];
    if (path[0] != '/') {
        /* prefix "/" without ever handing a truncated path to the sandbox
         * check below; the copy back is bounded by set_str */
        int w = snprintf(path_with_slash, sizeof path_with_slash, "/%s", path);
        if (w < 0 || (size_t)w >= sizeof path_with_slash) {
            sb_str(result, "error: path too long");
            return;
        }
        set_str(path, sizeof path, path_with_slash);
    }
    char real[MAX_PATH_SIZE];
    if (!resolve_within(g_web_root_real, path, real, sizeof real)) {
        sb_str(result, "error: path rejected (must stay inside the web root)");
        return;
    }
    FILE *f = fopen(real, "rb");
    if (!f) {
        sb_str(result, "error: cannot open '");
        sb_str(result, path);
        sb_str(result, "'");
        return;
    }
    size_t n = 0;
    char chunk[4096];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
        size_t take = r;
        if (n + take > TOOL_READ_MAX) take = (size_t)(TOOL_READ_MAX - n);
        sb_mem(result, chunk, take);
        n += take;
        if (n >= TOOL_READ_MAX) break;
    }
    fclose(f);
    if (result->oom) {
        sb_str(result, "error: out of memory reading file");
        return;
    }
    if (n == 0) {
        sb_str(result, "(empty file)");
    } else if (n >= TOOL_READ_MAX) {
        sb_str(result, "\n...[truncated at 8KB]");
    }
}

/* ---- fetch_url (fork curl, scheme-restricted, capped) --------------- */

/* SSRF guard: the LLM may pick any URL, so fail closed on loopback,
 * private, link-local and ULA targets before curl(1) is forked. Both the
 * literal address and every getaddrinfo result are checked (a hostname
 * resolving into 127.0.0.1 must not slip through; DNS rebinding inside the
 * 10s curl window is out of scope for this teaching server). */
/* Block loopback / private / link-local / reserved IPv4 ranges. Shared by the
 * literal AF_INET path and the IPv4-mapped/compatible forms of IPv6
 * (::ffff:a.b.c.d and ::a.b.c.d), which the kernel routes as IPv4. */
static int v4_blocked(const unsigned char *b) {
    /* 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10 (CGNAT), 127.0.0.0/8,
     * 169.254.0.0/16 (link-local / cloud metadata), 172.16.0.0/12,
     * 192.0.0.0/24, 192.0.2.0/24 (TEST-NET-1), 192.168.0.0/16,
     * 198.18.0.0/15 (benchmarking), 198.51.100.0/24 (TEST-NET-2),
     * 203.0.113.0/24 (TEST-NET-3) */
    if (b[0] == 0 || b[0] == 10 || b[0] == 127 ||
        (b[0] == 100 && (b[1] & 0xc0) == 64) ||
        (b[0] == 169 && b[1] == 254) ||
        (b[0] == 172 && (b[1] & 0xf0) == 16) ||
        (b[0] == 192 && (b[1] == 0 || b[1] == 2 || b[1] == 168)) ||
        (b[0] == 198 && (b[1] & 0xfe) == 18) ||
        (b[0] == 198 && b[1] == 51 && b[2] == 100) ||
        (b[0] == 203 && b[1] == 0 && b[2] == 113)) {
        return 1;
    }
    return 0;
}

static int addr_is_blocked(const struct sockaddr *sa, socklen_t salen) {
    (void)salen;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        const unsigned char *b = (const unsigned char *)&sin->sin_addr;
        return v4_blocked(b);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        const struct in6_addr *a = &sin6->sin6_addr;
        const unsigned char *b = (const unsigned char *)a;
        if (IN6_IS_ADDR_UNSPECIFIED(a) || IN6_IS_ADDR_LOOPBACK(a) ||
            IN6_IS_ADDR_LINKLOCAL(a) || IN6_IS_ADDR_SITELOCAL(a) ||
            (b[0] & 0xfe) == 0xfc) { /* fc00::/7 unique-local */
            return 1;
        }
        /* ::ffff:a.b.c.d (mapped) and ::a.b.c.d (compatible) are routed as
         * IPv4 by the kernel, so a literal [::ffff:169.254.169.254] would
         * otherwise slip past the v4 blocklist and reach the cloud metadata
         * service. Apply the same v4 guard to the embedded address. */
        if (IN6_IS_ADDR_V4MAPPED(a) || IN6_IS_ADDR_V4COMPAT(a)) {
            return v4_blocked(b + 12);
        }
    }
    return 0;
}

static int fetch_url_blocked(const char *url) {
    const char *scheme = strstr(url, "://");
    if (!scheme) return 0;
    const char *host = scheme + 3;
    const char *end = host;
    while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') end++;
    if (end == host) return 0;
    char hbuf[512];
    size_t hlen = (size_t)(end - host);
    if (hlen >= sizeof hbuf) hlen = sizeof hbuf - 1;
    memcpy(hbuf, host, hlen);
    hbuf[hlen] = '\0';

    /* Bare IP literals (incl. bracketed IPv6) — resolve free of DNS. */
    char ipbuf[64];
    const char *lit = hbuf[0] == '[' ? hbuf + 1 : hbuf;
    size_t litlen = hbuf[0] == '[' ? strlen(lit) : hlen;
    if (litlen > 0 && lit[litlen - 1] == ']') litlen--;
    if (litlen < sizeof ipbuf) {
        memcpy(ipbuf, lit, litlen);
        ipbuf[litlen] = '\0';
        struct in_addr a4;
        struct in6_addr a6;
        if (inet_pton(AF_INET, ipbuf, &a4) == 1) {
            struct sockaddr_in sin = {0};
            sin.sin_family = AF_INET;
            sin.sin_addr = a4;
            return addr_is_blocked((const struct sockaddr *)&sin, sizeof sin);
        }
        if (inet_pton(AF_INET6, ipbuf, &a6) == 1) {
            struct sockaddr_in6 sin6 = {0};
            sin6.sin6_family = AF_INET6;
            sin6.sin6_addr = a6;
            return addr_is_blocked((const struct sockaddr *)&sin6, sizeof sin6);
        }
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(hbuf, NULL, &hints, &res) != 0 || !res) return 0;
    int blocked = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (addr_is_blocked(ai->ai_addr, ai->ai_addrlen)) { blocked = 1; break; }
    }
    freeaddrinfo(res);
    return blocked;
}

static void tool_fetch_url(void *data, const char *args,
                          const char *session_id, sbuf *result) {
    (void)data;
    (void)session_id;
    char url[1024];
    if (tool_str_arg(args, "url", url, sizeof url) != 0) {
        sb_str(result, "error: missing string argument 'url'");
        return;
    }
    if (!(strncasecmp(url, "http://", 7) == 0 ||
          strncasecmp(url, "https://", 8) == 0) ||
        strpbrk(url, " \r\n\t\"'")) {
        sb_str(result, "error: url must be an absolute http(s) URL");
        return;
    }
    if (fetch_url_blocked(url)) {
        sb_str(result, "error: url resolves to a loopback/private network (blocked)");
        return;
    }

    int out_pipe[2];
    if (pipe(out_pipe) < 0) {
        sb_str(result, "error: pipe failed");
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        sb_str(result, "error: fork failed");
        return;
    }
    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execlp("curl", "curl", "-sS", "-L", "--max-time", "10",
               "--proto", "=http,https",
               "-A", "agent-httpd-agent/1.0", url, (char *)NULL);
        _exit(127);
    }
    close(out_pipe[1]);

    size_t n = 0;
    int failed = 0;
    char chunk[4096];
    for (;;) {
        ssize_t r = read(out_pipe[0], chunk, sizeof chunk);
        if (r < 0) {
            if (errno == EINTR) continue;
            failed = 1;
            break;
        }
        if (r == 0) break;
        size_t take = (size_t)r;
        if (n + take > TOOL_FETCH_MAX) take = (size_t)(TOOL_FETCH_MAX - n);
        sb_mem(result, chunk, take);
        n += take;
        if (n >= TOOL_FETCH_MAX) break; /* cap reached: stop draining */
    }
    close(out_pipe[0]);
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);

    if (result->oom) {
        sb_str(result, "error: out of memory");
        return;
    }
    if (n == 0 && (failed || WIFEXITED(status))) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        sb_str(result, code == 127
                           ? "error: curl(1) not found in PATH"
                           : "error: fetch returned no body (curl exit code in tool output above)");
    } else if (n >= TOOL_FETCH_MAX) {
        sb_str(result, "\n...[truncated at 16KB]");
    }
}

/* ---- skill-run ------------------------------------------------------ */

static void tool_skill_run(void *data, const char *args,
                          const char *session_id, sbuf *result) {
    (void)data;
    (void)session_id;
    char name[SKILL_NAME_MAX + 1];
    if (tool_str_arg(args, "skill", name, sizeof name) != 0) {
        sb_str(result, "error: missing string argument 'skill'");
        return;
    }
    char *body = skills_read(name);
    if (!body) {
        sb_str(result, "error: unknown skill '");
        sb_str(result, name);
        sb_str(result, "'");
        return;
    }
    if (strlen(body) > SKILL_BODY_MAX) {
        body[SKILL_BODY_MAX] = '\0';
        sb_str(result, "[skill content truncated]\n");
    }
    sb_str(result, name);
    sb_str(result, ":\n");
    sb_str(result, body);
    free(body);
}

/* ---- remember / recall (session memory) ----------------------------- */

static void tool_remember(void *data, const char *args,
                          const char *session_id, sbuf *result) {
    (void)data;
    char key[SESSION_KEY_MAX + 1], val[SESSION_VAL_MAX + 1];
    if (tool_str_arg(args, "key", key, sizeof key) != 0 &&
        tool_str_arg(args, "name", key, sizeof key) != 0) {
        sb_str(result, "error: missing string argument 'key'");
        return;
    }
    if (tool_str_arg(args, "value", val, sizeof val) != 0 &&
        tool_str_arg(args, "fact", val, sizeof val) != 0) {
        sb_str(result, "error: missing string argument 'value'");
        return;
    }
    if (session_fact_set_file(session_id, key, val) != 0) {
        sb_str(result, "error: failed to persist memory (bad key or disk)");
        return;
    }
    sb_str(result, "remembered ");
    sb_str(result, key);
    sb_str(result, " = ");
    sb_str(result, val);
}

static void tool_recall(void *data, const char *args,
                        const char *session_id, sbuf *result) {
    (void)data;
    char key[SESSION_KEY_MAX + 1];
    if (tool_str_arg(args, "key", key, sizeof key) != 0 &&
        tool_str_arg(args, "name", key, sizeof key) != 0) {
        sb_str(result, "error: missing string argument 'key'");
        return;
    }
    char val[SESSION_VAL_MAX + 3];
    if (session_fact_get_file(session_id, key, val, sizeof val) != 0) {
        sb_str(result, "error: no stored fact for '");
        sb_str(result, key);
        sb_str(result, "'");
        return;
    }
    sb_str(result, key);
    sb_str(result, " = ");
    sb_str(result, val);
}

/* ---- registry -------------------------------------------------------- */

void tools_init(void) {
    static const char *P_READ =
        "{\"path\":{\"type\":\"string\",\"description\":\"e.g. index.html or test/\"}}";
    static const char *P_SKILL =
        "{\"skill\":{\"type\":\"string\",\"description\":\"skill name from the index\"}}";
    static const char *P_MEM =
        "{\"key\":{\"type\":\"string\",\"description\":\"memory key (alnum, hyphen, underscore)\"},"
        "\"value\":{\"type\":\"string\",\"description\":\"fact to remember\"}}";
    static const char *P_KEY =
        "{\"key\":{\"type\":\"string\",\"description\":\"memory key (alnum, hyphen, underscore)\"}}";

    tools_register("get_time",
        "Get the server's current local date and time.", "{}",
        tool_get_time, NULL);
    tools_register("calc",
        "Evaluate an arithmetic expression (+ - * / % ^ and parentheses).",
        "{\"expression\":{\"type\":\"string\",\"description\":\"e.g. (2+3)*7\"}}",
        tool_calc, NULL);
    tools_register("read_file",
        "Read a file from the server's web root (www/). Paths are relative; traversal outside the web root is refused.",
        P_READ, tool_read_file, NULL);
    tools_register("fetch_url",
        "Fetch a web page or API over http(s) and return its body (first 16KB).",
        "{\"url\":{\"type\":\"string\",\"description\":\"absolute http(s) URL\"}}",
        tool_fetch_url, NULL);
    tools_register("skill-run",
        "Load a skill's full instructions from the skills index by name.",
        P_SKILL, tool_skill_run, NULL);
    tools_register("remember",
        "Store a fact in long-term memory (session-scoped; global pool when no session).",
        P_MEM, tool_remember, NULL);
    tools_register("recall",
        "Read a stored fact from long-term memory by key.",
        P_KEY, tool_recall, NULL);
}

int tools_dispatch(const char *name, const char *args_raw,
                   const char *session_id, sbuf *result) {
    for (int i = 0; i < g_ntools; i++) {
        if (strcmp(g_tools[i].name, name) == 0) {
            g_tools[i].fn(g_tools[i].data, args_raw, session_id, result);
            return 0;
        }
    }
    sb_str(result, "error: unknown tool '");
    sb_str(result, name);
    sb_str(result, "'");
    return -1;
}