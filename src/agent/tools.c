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
#include "sqlite_tool.h"

#define TOOL_READ_MAX 8192
#define TOOL_FETCH_MAX 16384
#define SKILL_BODY_MAX 16384

/* ---- registry ------------------------------------------------------ */

static ToolDef g_tools[TOOL_MAX];
static int g_ntools = 0;
/* Heap-owned cache: the old fixed TOOL_MAX*(...)+256 buffer silently
 * byte-truncated the tools array when the MCP fleet grew past it, and a
 * truncated tools array is invalid JSON that 400s every agent request. */
static char *g_schema_cache = NULL;
static int g_schema_dirty = 1;

static const char *tool_params_or_empty(const ToolDef *t) {
    return t->params[0] ? t->params : "{}";
}

const char *tools_schema_json(void) {
    if (!g_schema_dirty && g_schema_cache) return g_schema_cache;
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
    char *fresh = strdup(b.p ? b.p : "[]");
    free(b.p);
    if (fresh) {
        free(g_schema_cache);
        g_schema_cache = fresh;
        g_schema_dirty = 0;
    }
    /* on strdup failure keep serving the previous cache (still valid JSON)
     * and stay dirty so the next call retries */
    return g_schema_cache ? g_schema_cache : "[]";
}

/* Profile allow-list: HARNESS_TOOLS_ALLOW="a,b" restricts the agent tool
 * registry (builtins + DSL `tool` + MCP tools all flow through
 * tools_register); unset/empty allows everything. The OpenAI schema and the
 * DSL `tools()` readout are built from the same table, so a profile can slim
 * the whole surface an example exposes. */
int tools_register_allowed(const char *name) {
    if (!name) return 0;
    if (strstr(name, "__")) return 1; /* MCP tools are gated by MCP_ALLOW */
    const char *allow = getenv("HARNESS_TOOLS_ALLOW");
    if (!allow || !allow[0]) return 1;
    char buf[1024];
    set_str(buf, sizeof buf, allow);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        trim_whitespace(tok);
        if (strcmp(tok, name) == 0) return 1;
    }
    return 0;
}

int agenthttpd_tool_allowed(const char *name) {
    return tools_register_allowed(name);
}

int tools_register(const char *name, const char *desc, const char *params_json,
                   ToolFn fn, void *data) {
    if (!name || !fn || g_ntools >= TOOL_MAX) return -1;
    if (!tools_register_allowed(name)) {
        fprintf(stderr, "[tools] skip (profile) %s\n", name);
        return -1;
    }
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
    const char *params = params_json ? params_json : "{}";
    if (strlen(params) >= TOOL_PARAMS_MAX) {
        /* A byte-truncated params JSON is invalid JSON, and one invalid
         * entry poisons the whole tools array sent upstream (every agent
         * request 400s). Drop the params instead — the tool stays callable,
         * just without schema hints. */
        fprintf(stderr, "[tools] %s: params JSON too long (%zu > %d), "
                        "registering with {}\n", name, strlen(params),
                TOOL_PARAMS_MAX);
        params = "{}";
    }
    nb = utf8_valid_prefix(params, TOOL_PARAMS_MAX);
    memcpy(t->params, params, nb);
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
    /* The server runs in Shenzhen (China, UTC+8). Report Beijing time
     * explicitly rather than the container's local time: the Docker image
     * ships no tzdata and sets no TZ, so localtime_r yields UTC and would
     * mislead the operator by 8 hours. China keeps a fixed UTC+8 with no
     * DST, so a constant offset is correct. Shift the epoch and read it back
     * as a UTC wall clock — that wall clock IS the Beijing clock. */
    time_t beijing = now + 8 * 3600;
    struct tm tmv;
    gmtime_r(&beijing, &tmv);
    strftime(human, sizeof human, "%Y-%m-%d %H:%M:%S", &tmv);
    format_http_date(http, sizeof http, now);
    sb_str(result, "server time (Beijing, UTC+8): ");
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
    if (!scheme) return 0;                 /* not our scheme; tool_fetch_url
                                             * rejects it before we get here */
    const char *host = scheme + 3;
    /* Strip userinfo (RFC 3986 "userinfo@host"): an attacker could hide the
     * real target behind an '@' so a naive parser connects to the wrong host,
     * e.g. http://benign@169.254.169.254/ . Stripping first means the
     * literal-IP / IPv6 fast paths and the DNS lookup below all see the true
     * host instead of relying on the lookup to fail. */
    const char *at = host;
    while (*at && *at != '/' && *at != '?' && *at != '#' && *at != '@') at++;
    if (*at == '@') host = at + 1;

    /* Locate the end of the authority. A bracketed IPv6 literal contains ':'
     * characters, so stop at ']' for it; otherwise stop at the first '/',
     * ':' (port separator, excluded from the lookup), '?' or '#'. Without the
     * bracket handling, http://[::1]/ was never blocked because the embedded
     * ':' truncated the host and getaddrinfo then failed open. */
    const char *end;
    if (*host == '[') {
        const char *cb = strchr(host, ']');
        end = cb ? cb + 1 : host + strlen(host);
    } else {
        end = host;
        while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') end++;
    }
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

    /* Hostname: every resolved address is checked. DNS failure is FAIL-CLOSED
     * — an unresolvable or attacker-controlled name must not be fetched, so a
     * resolution error blocks the request (a redirect/SSRF probe that points
     * at a name we cannot resolve is stopped here; the message below covers
     * both the loopback and the unresolvable cases). */
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(hbuf, NULL, &hints, &res) != 0 || !res) return 1;
    int blocked = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (addr_is_blocked(ai->ai_addr, ai->ai_addrlen)) { blocked = 1; break; }
    }
    freeaddrinfo(res);
    return blocked;
}

/* ---- fetch_url redirect handling (SSRF-safe) ------------------------- */

/* Case-insensitive lookup of header `name` in an HTTP header block; copies the
 * trimmed value (sans CRLF) into out. Returns 1 on hit. */
static int http_header_value(const char *hdr, const char *name,
                             char *out, size_t outsz) {
    size_t nl = strlen(name);
    const char *p = hdr;
    while ((p = strcasestr(p, name)) != NULL) {
        if ((p == hdr || p[-1] == '\n') && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *e = v;
            while (*e && *e != '\r' && *e != '\n') e++;
            size_t len = (size_t)(e - v);
            if (len >= outsz) len = outsz - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return 1;
        }
        p += nl;
    }
    return 0;
}

/* 3-digit status code from an HTTP status line ("HTTP/1.1 301 Moved"). */
static int http_status_code(const char *hdr) {
    const char *sp = hdr;
    while (*sp && *sp != ' ' && *sp != '\t') sp++;
    if (!*sp) return 0;
    sp++;
    if (!isdigit((unsigned char)*sp)) return 0;
    int code = atoi(sp);
    return (code >= 100 && code < 600) ? code : 0;
}

/* scheme://host[:port] of a URL (its origin), for resolving relative
 * redirects. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void url_origin(const char *url, char *out, size_t outsz) {
    const char *s = strstr(url, "://");
    if (!s) { snprintf(out, outsz, "%s", "http://"); return; }
    const char *host = s + 3;
    const char *end = host;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    size_t n = (size_t)(end - url);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, url, n);
    out[n] = '\0';
}

/* Resolve a redirect Location against `base` into an absolute URL in `out`.
 * Handles absolute, protocol-relative (//host) and origin-relative (/path)
 * targets; anything else is treated as origin-relative for safety. The result
 * is re-validated by fetch_url_blocked() on the next hop, so a resolution bug
 * here can never open a hole — it can only fail closed. snprintf truncates
 * safely, so the format-truncation diagnostic is intentionally suppressed. */
static void resolve_redirect(const char *base, const char *loc,
                             char *out, size_t outsz) __attribute__((noinline));
static void resolve_redirect(const char *base, const char *loc,
                             char *out, size_t outsz) {
    if (strncasecmp(loc, "http://", 7) == 0 ||
        strncasecmp(loc, "https://", 8) == 0) {
        snprintf(out, outsz, "%s", loc);
        return;
    }
    if (loc[0] == '/' && loc[1] == '/') {
        const char *s = strstr(base, "://");
        const char *scheme = s ? base : "http:";
        size_t sl = s ? (size_t)(s - base + 3) : 5;
        snprintf(out, outsz, "%.*s%s", (int)sl, scheme, loc);
        return;
    }
    char origin[1024];
    url_origin(base, origin, sizeof origin);
    if (loc[0] == '/') {
        snprintf(out, outsz, "%s%s", origin, loc);
    } else {
        snprintf(out, outsz, "%s/%s", origin, loc);
    }
}

#pragma GCC diagnostic pop

#define TOOL_FETCH_MAX_HOPS 5
#define TOOL_FETCH_READ_MAX (TOOL_FETCH_MAX + 4096)

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

    /* Redirect loop: curl is launched WITHOUT -L so we follow 3xx responses
     * ourselves and re-validate every hop with fetch_url_blocked(). A bare
     * curl -L would follow a redirect straight into an internal address
     * (e.g. http://evil/ -> 302 -> http://169.254.169.254/) with no second
     * SSRF check. */
    char cur[2048];
    snprintf(cur, sizeof cur, "%s", url);
    sbuf buf = {0};
    int hop = 0;
    for (;;) {
        if (fetch_url_blocked(cur)) {
            sb_str(result,
                   "error: url is blocked — unresolvable or targets a "
                   "loopback/private network");
            free(buf.p);
            return;
        }

        int out_pipe[2];
        if (pipe(out_pipe) < 0) {
            sb_str(result, "error: pipe failed");
            free(buf.p);
            return;
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(out_pipe[0]);
            close(out_pipe[1]);
            sb_str(result, "error: fork failed");
            free(buf.p);
            return;
        }
        if (pid == 0) {
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(out_pipe[1], STDERR_FILENO);
            close(out_pipe[0]);
            close(out_pipe[1]);
            /* -D - dumps the response headers to stdout so we can read the
             * Location header; we then decide whether to follow, re-checking. */
            execlp("curl", "curl", "-sS", "--http1.1", "--max-time", "10",
                   "--proto", "=http,https", "-D", "-",
                   "-A", "agent-httpd-agent/1.0", cur, (char *)NULL);
            _exit(127);
        }
        close(out_pipe[1]);

        buf.p = NULL; buf.len = 0; buf.oom = 0;
        char chunk[4096];
        for (;;) {
            ssize_t r = read(out_pipe[0], chunk, sizeof chunk);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r == 0) break;
            size_t take = (size_t)r;
            if (buf.len + take > TOOL_FETCH_READ_MAX) {
                take = TOOL_FETCH_READ_MAX - buf.len;
            }
            if (take) sb_mem(&buf, chunk, take);
            if (buf.len >= TOOL_FETCH_READ_MAX || buf.oom) break;
        }
        close(out_pipe[0]);
        kill(pid, SIGKILL);
        /* SIGCHLD is SIG_IGN process-wide (main.c): the kernel reaps curl as
         * it exits and waitpid() comes back ECHILD with `status` untouched. A
         * zeroed status reads as "exited 0", so only trust the exit code when
         * we actually reaped (same guard as agent.c / router.c). */
        int status = 0;
        pid_t reaped;
        do {
            reaped = waitpid(pid, &status, 0);
        } while (reaped < 0 && errno == EINTR);
        int code_known = (reaped == pid);

        if (buf.oom) {
            sb_str(result, "error: out of memory");
            free(buf.p);
            return;
        }

        /* Split headers / body at the first blank line. */
        char *hdr = buf.p ? buf.p : (char *)"";
        char *body_start = buf.p ? buf.p + buf.len : (char *)"";
        char *h_end = buf.p ? strstr(buf.p, "\r\n\r\n") : NULL;
        if (h_end) body_start = h_end + 4;
        else if (buf.p) {
            char *h_end2 = strstr(buf.p, "\n\n");
            if (h_end2) body_start = h_end2 + 2;
        }

        int code = http_status_code(hdr);
        char loc[2048];
        if (code >= 300 && code < 400 && hop < TOOL_FETCH_MAX_HOPS &&
            http_header_value(hdr, "location", loc, sizeof loc)) {
            char next[2048];
            resolve_redirect(cur, loc, next, sizeof next);
            snprintf(cur, sizeof cur, "%s", next);
            free(buf.p);
            hop++;
            continue;   /* re-validate `next` at the top of the loop */
        }

        /* Final (non-redirect, or hop cap reached) response: emit the body,
         * capped at TOOL_FETCH_MAX. */
        size_t blen = 0;
        if (buf.p && body_start >= buf.p && body_start <= buf.p + buf.len) {
            blen = (size_t)(buf.p + buf.len - body_start);
        }
        if (blen > 0) {
            size_t take = blen;
            if (take > TOOL_FETCH_MAX) take = TOOL_FETCH_MAX;
            sb_mem(result, body_start, take);
            if (blen > TOOL_FETCH_MAX) sb_str(result, "\n...[truncated at 16KB]");
        } else if (code_known && (code == 0 || !WIFEXITED(status) ||
                   WEXITSTATUS(status) != 0)) {
            sb_str(result, WEXITSTATUS(status) == 127
                           ? "error: curl(1) not found in PATH"
                           : "error: fetch returned no body (curl exit code in tool output above)");
        }
        free(buf.p);
        break;
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
    /* H2: memory is session-scoped. Without a sessionId the fallback store
     * is a single global pool shared by every anonymous request, which
     * leaks one user's facts into another's context (and is a prompt-
     * injection sink). Refuse rather than write to the global pool. */
    if (!session_id || !session_id[0]) {
        sb_str(result, "error: memory requires a session; pass a sessionId in the request (memory is not global)");
        return;
    }
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
    /* H2: see tool_remember — never read from the shared global pool. */
    if (!session_id || !session_id[0]) {
        sb_str(result, "error: memory requires a session; pass a sessionId in the request (memory is not global)");
        return;
    }
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

    /* native SQLite (registers nothing when SQLITE_DB is unset) */
    sqlite_tools_init();
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