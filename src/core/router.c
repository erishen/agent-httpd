#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "router.h"
#include "llm.h"
#include "minijson.h"
#include "tools.h"

/* Catalog responses are small (a few KB per skill body); cap defensively. */
#define ROUTER_JSON_MAX (1024 * 1024)
#define ROUTER_PATH_MAX (1024)

/* Whole-sync time budget. A half-dead router (TCP accept then silence —
 * the rolling-restart window) makes every catalog request eat its full
 * curl timeout; with N skills that serializes into N x 8s of startup
 * delay, which Docker healthchecks misread as a failed boot. The sync is
 * optional sugar (stale skills/mcps just mean yesterday's catalog), so it
 * gets a hard wall-clock budget instead: past it, remaining fetches are
 * skipped and the server continues starting. Overridable via
 * ROUTER_SYNC_BUDGET_SECONDS; 0 disables the budget. */
#define ROUTER_SYNC_BUDGET_DEFAULT 10
static time_t g_sync_deadline = 0;

static int sync_budget_expired(void) {
    if (!g_sync_deadline) return 0;
    return time(NULL) >= g_sync_deadline;
}

static int sync_seconds_left(void) {
    if (!g_sync_deadline) return 8; /* no budget: per-request default */
    long left = (long)(g_sync_deadline - time(NULL));
    if (left < 1) return 0;
    return (int)left;
}

/* Spawn recipe for MCP servers the router mounts but refuses to expose.
 * fs needs a sandbox root (MCP_FS_ROOT); without one we skip it fail-closed
 * rather than handing the model read/write on an arbitrary directory. */
typedef struct {
    const char *id;
    const char *command;
    const char *args; /* NULL -> no args; "$FSROOT" substituted for fs */
} RouterMcpTemplate;

/* Local spawn recipes for the MCP servers the router mounts. Versions are
 * pinned so the Docker image can pre-warm the exact same specs into its npx
 * cache (see Dockerfile) — an unpinned "latest" re-resolves on every boot and
 * re-downloads whenever upstream publishes, which on a cold C-network easily
 * blows the MCP init budget and fails every handshake ("[mcp] ... handshake
 * failed"). Keep these versions in sync with the Dockerfile pre-warm step. */
static const RouterMcpTemplate k_known_mcps[] = {
    {"fs", "npx", "-y @modelcontextprotocol/server-filesystem@2026.8.31 $FSROOT"},
    {"think", "npx", "-y @modelcontextprotocol/server-sequential-thinking@2026.8.31"},
    {"memory", "npx", "-y @modelcontextprotocol/server-memory@2026.8.31"},
};
#define KNOWN_MCPS (sizeof k_known_mcps / sizeof k_known_mcps[0])

static const char *router_base(void) {
    const char *u = getenv("ROUTER_API_URL");
    if (u && *u) return u;
    /* Default: the same gateway the agent already talks to. A direct
     * provider URL simply won't answer /v1/mcps like a router -> no-op. */
    return getenv("LLM_API_URL");
}

/* Fork curl(1) with a fixed argv, read its stdout+stderr into a malloc'd
 * string, and reap it. Shared by GET (catalog) and POST (tool invoke).
 * `left` is the remaining wall-clock budget in seconds; `post_body` non-NULL
 * switches to an application/json POST. Returns NULL on transport failure or
 * an empty body. */
static char *router_curl_exec(const char *url, const char *auth, int left,
                              const char *post_body) {
    int p[2];
    if (pipe(p) < 0) {
        perror("[router] pipe");
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("[router] fork");
        close(p[0]);
        close(p[1]);
        return NULL;
    }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        close(p[0]);
        close(p[1]);
        /* curl's own cap rides the remaining budget (min 1s, max the usual
         * 8s) so one request can never spend more than what's left of the
         * wall. */
        char mt[16];
        snprintf(mt, sizeof mt, "%d", left > 8 ? 8 : (left < 1 ? 1 : left));
        if (post_body) {
            execlp("curl", "curl", "-sS", "--http1.1", "-L", "--max-time", mt,
                   "--proto", "=http,https", "-X", "POST",
                   "-H", auth, "-H", "Content-Type: application/json",
                   "-d", post_body, url, (char *)NULL);
        } else {
            execlp("curl", "curl", "-sS", "--http1.1", "-L", "--max-time", mt,
                   "--proto", "=http,https", "-H", auth, url, (char *)NULL);
        }
        _exit(127);
    }
    close(p[1]);

    /* The router answers JSON identically to /v1/models-style responses, so
     * only proxy non-2xx out of band isn't exposed; curl body is still the
     * JSON. 404/401 shows up as {"error":...} and we treat that as
     * "not a router / must stay quiet". */
    sbuf b = {0};
    size_t n = 0;
    char chunk[4096];
    for (;;) {
        ssize_t r = read(p[0], chunk, sizeof chunk);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        sb_mem(&b, chunk, (size_t)r);
        n += (size_t)r;
        if (n >= ROUTER_JSON_MAX) break;
    }
    close(p[0]);
    kill(pid, SIGKILL);
    /* Reap defensively: SIGKILL delivery has a tiny window, and macOS has
     * surprising SIGCHLD/SIG_IGN reaping semantics that can leave a plain
     * waitpid() parked on an already-gone pid. Poll with WNOHANG + a short
     * hard cap (2s, way past any kill latency) so catalog sync can never
     * wedge the boot here either. */
    {
        int waited = 0;
        for (int i = 0; i < 200; i++) {
            pid_t r = waitpid(pid, NULL, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD)) {
                waited = 1;
                break;
            }
            usleep(10 * 1000);
        }
        (void)waited;
    }

    if (b.oom || !b.p || !*b.p) {
        free(b.p);
        return NULL;
    }
    return b.p;
}

/* GET <base>/<endpoint> with the chat key. Returns a malloc'd body (caller
 * frees) or NULL on transport failure. */
static char *router_get(const char *endpoint, int *have) {
    *have = 0;
    const char *base = router_base();
    const char *key = getenv("LLM_API_KEY");
    if (!base || !*base || !key || !*key) return NULL;
    /* Budget wall: past the sync deadline, don't even fork — a stale
     * catalog is fine, a stalled boot is not. */
    if (sync_budget_expired()) {
        fprintf(stderr, "[router] sync budget exhausted, skipping %s\n",
                endpoint);
        return NULL;
    }
    int left = sync_seconds_left();
    if (left < 1) return NULL;

    char url[ROUTER_PATH_MAX];
    if (snprintf(url, sizeof url, "%s/%s", base, endpoint) >= (int)sizeof url) {
        fprintf(stderr, "[router] endpoint URL too long: %s\n", endpoint);
        return NULL;
    }
    char auth[256];
    if (snprintf(auth, sizeof auth, "Authorization: Bearer %s", key) >= (int)sizeof auth) {
        fprintf(stderr, "[router] key too long\n");
        return NULL;
    }
    char *body = router_curl_exec(url, auth, left, NULL);
    if (body) *have = 1;
    return body;
}

/* One skill entry from the router's /skills list. */
typedef struct {
    char *name;
    char *desc; /* "" when the list omits a description */
} RouterSkill;

/* jread_string fills a fixed-size buffer; a long multi-byte description is
 * then cut at the byte limit and can end mid-character, leaving invalid
 * UTF-8 in the generated SKILL.md (and later in the agent's system prompt).
 * Trim the buffer back to the last complete UTF-8 character — but only when
 * the tail actually IS an incomplete character. The old loop popped trailing
 * continuation bytes and then dropped whatever lead byte it found, which
 * wrongly ate the final CJK character of every desc that legitimately ends
 * in one (「质量」→「质」). Judge by code-point width instead: back up over
 * continuation bytes to the lead, and truncate only if it lacks the bytes a
 * properly started sequence needs. */
static void utf8_trim_tail(char *s) {
    size_t n = strlen(s);
    if (n == 0) return;
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    unsigned char b = (unsigned char)s[i];
    size_t width;
    if (b >= 0xF0) width = 4;
    else if (b >= 0xE0) width = 3;
    else if (b >= 0xC2) width = 2;
    else width = 1; /* ASCII: the byte itself is a complete code point */
    if (i + width != n) s[i] = '\0'; /* incomplete trailing char -> drop it */
}

/* Parse `{"skills":[{name,description},...]}` into a malloc'd array of
 * RouterSkill. *out = count; NULL on transport/parse failure. */
static RouterSkill *parse_skills(const char *json, int *out) {
    *out = 0;
    const char *sk = jfind_value(json, "skills");
    if (!sk || *sk != '[') return NULL;
    const char *p = sk + 1;
    RouterSkill *items = NULL;
    int n = 0;
    for (;;) {
        p = jws(p);
        if (*p == ']' || !*p) break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        char obj[4096];
        const char *q = p;
        const char *end = q;
        /* grab one object: walk balanced braces */
        int depth = 0;
        for (; *end; end++) {
            if (*end == '{') depth++;
            else if (*end == '}') { depth--; if (depth == 0) { end++; break; } }
        }
        int slen = (int)(end - q);
        if (slen <= 0 || slen >= (int)sizeof obj) break;
        memcpy(obj, q, (size_t)slen);
        obj[slen] = '\0';
        p = end;

        RouterSkill s;
        memset(&s, 0, sizeof s);
        const char *nv = jfind_value(obj, "name");
        if (nv && *nv == '"') {
            char name[256];
            const char *np = nv;
            if (jread_string(&np, name, sizeof name)) s.name = strdup(name);
        }
        if (!s.name) continue; /* needs a name to materialize under */
        const char *dv = jfind_value(obj, "description");
        if (dv && *dv == '"') {
            /* weekly-investment's CJK description alone is ~300 bytes; keep
             * the cap comfortably larger and never emit a mid-char cut. */
            char desc[512];
            const char *dp = dv;
            if (jread_string(&dp, desc, sizeof desc)) {
                utf8_trim_tail(desc);
                s.desc = strdup(desc);
            }
        }
        if (!s.desc) s.desc = strdup("");
        if (!s.desc) { free(s.name); continue; }

        RouterSkill *grown = realloc(items, sizeof(RouterSkill) * (size_t)(n + 1));
        if (!grown) { free(s.name); free(s.desc); break; }
        items = grown;
        items[n++] = s;
    }
    *out = n;
    return items;
}

/* Write one skill body to skills/router/<name>/SKILL.md. `body` is the raw
 * JSON string value (opening quote included); unescaped once here. The
 * router's list carries a description — we bake it into a token-minimal
 * frontmatter so skills.c (name/description from the --- block) indexes it
 * and the skill's desc survives into the system prompt and skills(). */
static void materialize_skill(const char *name, const char *desc,
                              const char *body) {
    if (!name || !*name || !body || !*body) return;
    /* name arrives from the router /skills response, NOT from skills.c's own
     * loader, so do not trust the "dir-safe by construction" assumption there.
     * Reject any separator or ".." component to prevent writing outside the
     * skills/ subtree (e.g. "../../etc/cron.d/x"). */
    if (strpbrk(name, "/\\") || strstr(name, "..")) {
        fprintf(stderr, "[router] skill name rejected (path separator): %s\n", name);
        return;
    }
    char dir[ROUTER_PATH_MAX];
    if (snprintf(dir, sizeof dir, "skills/router/%s", name) >= (int)sizeof dir) return;
    /* A fresh project may not have skills/ yet; mkdir("skills/router") would
     * then fail with ENOENT and silently drop the whole catalog. Create the
     * parent first (each level idempotent). */
    if (mkdir("skills", 0755) != 0 && errno != EEXIST) return;
    if (mkdir("skills/router", 0755) != 0 && errno != EEXIST) return;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return;

    char path[ROUTER_PATH_MAX];
    if (snprintf(path, sizeof path, "%s/SKILL.md", dir) >= (int)sizeof path) return;
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[router] cannot write skill %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "---\nname: %s\n", name);
    if (desc && *desc) {
        /* skills.c reads value = rest of line: keep it on one line */
        fputs("description: ", f);
        for (const char *d = desc; *d; d++)
            fputc((*d == '\n' || *d == '\r') ? ' ' : *d, f);
        fputc('\n', f);
    }
    fputs("---\n\n", f);
    if (*body == '"') {
        const char *p = body;
        char *plain = malloc(strlen(body));
        if (plain && jread_string(&p, plain, strlen(body))) {
            fputs(plain, f);
        } else {
            fputs(body + 1, f); /* malformed: emit raw, salvagable */
        }
        free(plain);
    } else {
        fputs(body, f);
    }
    fclose(f);
}

/* Sync the MCP servers the router mounts into .data/mcp-servers-router.json
 * (read by mcp_config_load alongside the operator-owned file). */
static void sync_mcps(void) {
    int have = 0;
    char *json = router_get("mcps", &have);
    if (!have || !json) {
        free(json);
        return;
    }
    const char *arr = jfind_value(json, "mcps");
    if (!arr || *arr != '[') {
        free(json);
        return;
    }
    const char *p = arr + 1;
    FILE *f = fopen(".data/mcp-servers-router.json", "w");
    if (!f) {
        fprintf(stderr, "[router] cannot write .data/mcp-servers-router.json: %s\n",
                strerror(errno));
        free(json);
        return;
    }
    fchmod(fileno(f), 0600);  /* 私密配置:不随 umask 落 0644 */
    fputs("[\n", f);
    int wrote = 0;
    for (;;) {
        p = jws(p);
        if (*p == ']' || !*p) break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        char obj[4096];
        const char *q = p;
        const char *end = q;
        int depth = 0;
        for (; *end; end++) {
            if (*end == '{') depth++;
            else if (*end == '}') { depth--; if (depth == 0) { end++; break; } }
        }
        int slen = (int)(end - q);
        if (slen <= 0 || slen >= (int)sizeof obj) break;
        memcpy(obj, q, (size_t)slen);
        obj[slen] = '\0';
        p = end;

        const char *nv = jfind_value(obj, "name");
        char name[128] = "";
        if (nv && *nv == '"') {
            const char *np = nv;
            if (!jread_string(&np, name, sizeof name)) continue;
        }
        if (!*name) continue;

        /* locate spawn recipe */
        const RouterMcpTemplate *t = NULL;
        for (size_t i = 0; i < KNOWN_MCPS; i++) {
            if (strcmp(k_known_mcps[i].id, name) == 0) { t = &k_known_mcps[i]; break; }
        }
        if (!t) {
            fprintf(stderr, "[router] mcp '%s': no local spawn recipe, skip\n", name);
            continue;
        }
        char args[ROUTER_PATH_MAX] = "";
        if (t->args) {
            if (strstr(t->args, "$FSROOT")) {
                const char *root = getenv("MCP_FS_ROOT");
                if (!root || !*root) {
                    fprintf(stderr, "[router] mcp 'fs': MCP_FS_ROOT unset, skip\n");
                    continue;
                }
                /* 替换模板里的 $FSROOT 占位符（而不是复制一份 spec），
                 * 版本钉在模板里、这里只换路径，两边不会漂移。 */
                const char *ph = strstr(t->args, "$FSROOT");
                size_t pre = (size_t)(ph - t->args);
                if (pre + strlen(root) + strlen(ph + 7) >= sizeof args) continue;
                memcpy(args, t->args, pre);
                strcpy(args + pre, root);
                strcat(args, ph + 7);
            } else {
                if (snprintf(args, sizeof args, "%s", t->args) >= (int)sizeof args) continue;
            }
        }
        if (wrote) fputs(",\n", f);
        sbuf b = {0};
        sb_str(&b, "  {\"id\": ");
        sb_json_str(&b, name);
        sb_str(&b, ", \"transport\": \"stdio\", \"command\": ");
        sb_json_str(&b, t->command);
        sb_str(&b, ", \"args\": ");
        sb_json_str(&b, args);
        sb_str(&b, ", \"approval\": false}");
        if (b.p) fputs(b.p, f);
        free(b.p);
        wrote++;
    }
    fputs("\n]\n", f);
    fclose(f);
    printf("llm-router mcps: %d server(s) synced -> .data/mcp-servers-router.json\n", wrote);
    free(json);
}

/* Sync the skill library into skills/router/<name>/SKILL.md. */
static void sync_skills(void) {
    int have = 0;
    char *json = router_get("skills", &have);
    if (!have || !json) {
        free(json);
        return;
    }
    RouterSkill *skills = NULL;
    int n = 0;
    skills = parse_skills(json, &n);
    free(json);
    if (!skills) return;

    int ok = 0;
    for (int i = 0; i < n && skills[i].name; i++) {
        char ep[ROUTER_PATH_MAX];
        /* names are alnum/dash only, no URL-encoding needed */
        if (snprintf(ep, sizeof ep, "skills/%s", skills[i].name) >=
            (int)sizeof ep) continue;
        int h2 = 0;
        char *body = router_get(ep, &h2);
        if (!h2 || !body) {
            free(body);
            continue;
        }
        /* body key */
        const char *bv = jfind_value(body, "body");
        if (bv) {
            if (*bv == '"') {
                materialize_skill(skills[i].name, skills[i].desc, bv);
            } else {
                fprintf(stderr, "[router] skill '%s' body not a string, skip\n",
                        skills[i].name);
            }
        }
        free(body);
        ok++;
    }
    for (int i = 0; i < n; i++) {
        free(skills[i].name);
        free(skills[i].desc);
    }
    free(skills);
    printf("llm-router skills: %d skill(s) synced -> skills/router/\n", ok);
}

/* ---- router tool proxy -------------------------------------------------
 *
 * The router lists builtin/conditional tools (calc, echo, fetch_url,
 * get_time, query_exchange_rate, system_info, execute_code, ...) in
 * GET /v1/tools, but only EXECUTES them on the no-tools chat path. To make
 * them usable inside our own ReAct loop we register each as a local proxy
 * tool whose handler POSTs {"name","args"} to /v1/tools/invoke and relays
 * the returned "result" text. Names already claimed by a local builtin
 * (calc/get_time/fetch_url/skill-run/remember/recall/read_file) lose the
 * registration race by design: ours run locally and need no round-trip.
 * MCP-derived entries (source "mcp:*") are skipped — those servers are
 * already mounted locally via /v1/mcps, so the router aliases would only
 * duplicate them. */

#define ROUTER_TOOL_MAX 64

typedef struct {
    char *name;
    char *desc;
    char *params;
} RouterTool;

static RouterTool g_router_tools[ROUTER_TOOL_MAX];
static int g_router_tools_n = 0;

static void router_tools_reset(void) {
    for (int i = 0; i < g_router_tools_n; i++) {
        free(g_router_tools[i].name);
        free(g_router_tools[i].desc);
        free(g_router_tools[i].params);
    }
    g_router_tools_n = 0;
}

/* Unescape the JSON string under `key` (top-level) into a fresh string. */
static char *json_string_dup(const char *obj, const char *key) {
    const char *v = jfind_value(obj, key);
    if (!v || *v != '"') return NULL;
    size_t cap = strlen(v) + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    const char *p = v;
    if (!jread_string(&p, out, cap)) {
        free(out);
        return NULL;
    }
    return out;
}

/* Copy the raw JSON value under `key` (top-level) as text. */
static char *json_value_dup(const char *obj, const char *key) {
    const char *v = jfind_value(obj, key);
    if (!v) return NULL;
    const char *q = v;
    if (jskip_value(&q) != 0) return NULL;
    size_t len = (size_t)(q - v);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, v, len);
    out[len] = '\0';
    return out;
}

static void add_router_tool(const char *obj) {
    if (g_router_tools_n >= ROUTER_TOOL_MAX) return;
    char *name = json_string_dup(obj, "name");
    if (!name || !*name) {
        free(name);
        return;
    }
    char *source = json_string_dup(obj, "source");
    if (source && strncmp(source, "mcp:", 4) == 0) {
        /* mounted locally already; the router's mcp_* alias is a duplicate */
        free(name);
        free(source);
        return;
    }
    free(source);
    char *desc = json_string_dup(obj, "description");
    char *params = json_value_dup(obj, "parameters");
    RouterTool *t = &g_router_tools[g_router_tools_n];
    t->name = name;
    t->desc = desc ? desc : strdup("");
    t->params = params ? params : strdup("{}");
    g_router_tools_n++;
}

/* GET /v1/tools and stash the catalog for router_register_tools(). Runs in
 * router_sync_all() (before tools_init), so registration is deferred until
 * the local builtins exist and win any name collision. */
static void sync_tools(void) {
    int have = 0;
    char *json = router_get("tools", &have);
    if (!have || !json) {
        free(json);
        return;
    }
    router_tools_reset();
    const char *arr = jfind_value(json, "tools");
    if (!arr || *arr != '[') {
        free(json);
        return;
    }
    /* Slice one balanced {...} at a time in place (the buffer is ours and
     * mutable): temporarily NUL out the '}' terminator, parse, restore.
     * String contents are skipped so a brace inside a description cannot
     * desync the walk. */
    char *p = (char *)arr + 1;
    for (;;) {
        p = (char *)jws(p);
        if (*p == ']' || !*p) break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        int depth = 0;
        char *end = p;
        int ok = 0;
        for (; *end; end++) {
            if (*end == '"') {
                end++;
                while (*end && *end != '"') {
                    if (*end == '\\' && end[1]) end++;
                    end++;
                }
                if (!*end) break;
            } else if (*end == '{') {
                depth++;
            } else if (*end == '}') {
                depth--;
                if (depth == 0) { end++; ok = 1; break; }
            }
        }
        if (!ok) break;
        char saved = *end;
        *end = '\0';
        add_router_tool(p);
        *end = saved;
        p = end;
    }
    free(json);
    printf("llm-router tools: %d tool(s) synced (mcp:* skipped)\n",
           g_router_tools_n);
}

/* POST {"name":<tool>,"args":<args_json>} to <base>/tools/invoke. */
static char *router_invoke(const char *tool, const char *args_json) {
    const char *base = router_base();
    const char *key = getenv("LLM_API_KEY");
    if (!base || !*base || !key || !*key) return NULL;
    /* At runtime g_sync_deadline is 0, so this is the usual 8s per-request
     * cap; it only matters if a resync happens to be in flight. */
    int left = sync_seconds_left();
    if (left < 1) left = 8;

    char url[ROUTER_PATH_MAX];
    if (snprintf(url, sizeof url, "%s/tools/invoke", base) >= (int)sizeof url)
        return NULL;
    char auth[256];
    if (snprintf(auth, sizeof auth, "Authorization: Bearer %s", key) >=
        (int)sizeof auth)
        return NULL;

    sbuf body = {0};
    sb_str(&body, "{\"name\":");
    sb_json_str(&body, tool);
    sb_str(&body, ",\"args\":");
    sb_str(&body, (args_json && *args_json) ? args_json : "{}");
    sb_str(&body, "}");
    if (body.oom || !body.p) {
        free(body.p);
        return NULL;
    }
    char *resp = router_curl_exec(url, auth, left, body.p);
    free(body.p);
    return resp;
}

/* ToolFn for a router proxy tool (data == strdup'd remote tool name). */
static void router_tool_proxy(void *data, const char *args_json,
                              const char *session_id, sbuf *out) {
    (void)session_id; /* memory isolation lives on the router key, not here */
    const char *tool = (const char *)data;
    char *resp = router_invoke(tool, args_json);
    if (!resp) {
        sb_str(out, "error: router tool '");
        sb_str(out, tool);
        sb_str(out, "' failed (router unreachable or timed out)");
        return;
    }
    const char *rv = jfind_value(resp, "result");
    if (rv && *rv == '"') {
        size_t cap = strlen(rv) + 1;
        char *plain = malloc(cap);
        if (plain) {
            const char *p = rv;
            if (jread_string(&p, plain, cap)) {
                sb_str(out, plain);
            } else {
                sb_str(out, "error: malformed response from router tool '");
                sb_str(out, tool);
                sb_str(out, "'");
            }
            free(plain);
        }
    } else {
        const char *ev = jfind_value(resp, "error");
        const char *mv = ev ? jfind_value(ev, "message") : NULL;
        sb_str(out, "error: router tool '");
        sb_str(out, tool);
        sb_str(out, "' rejected");
        if (mv && *mv == '"') {
            char msg[256];
            const char *mp = mv;
            if (jread_string(&mp, msg, sizeof msg)) {
                sb_str(out, ": ");
                sb_str(out, msg);
            }
        }
    }
    free(resp);
}

/* Register the stashed router catalog as local proxy tools. Call AFTER
 * tools_init()/mcp_init() so local builtins win name collisions. Duplicates
 * (local or from a prior registration) are dropped. */
void router_register_tools(void) {
    int ok = 0;
    for (int i = 0; i < g_router_tools_n; i++) {
        RouterTool *t = &g_router_tools[i];
        char *nm = strdup(t->name);
        if (!nm) continue;
        if (tools_register(nm, t->desc, t->params, router_tool_proxy, nm) == 0) {
            ok++;
        } else {
            free(nm);
        }
    }
    if (g_router_tools_n > 0) {
        printf("llm-router tools: %d/%d registered locally (local builtins win)\n",
               ok, g_router_tools_n);
        fflush(stdout);
    }
}

void router_sync_all(void) {
    llm_env_init(); /* LLM_API_URL/key live in .env and load lazily otherwise */
    const char *base = router_base();
    if (!base || !*base) return;
    if (mkdir(".data", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[router] cannot create .data: %s\n", strerror(errno));
        return;
    }
    /* Arm the whole-sync budget (0 = unlimited, keeping old behavior for
     * anyone who really wants a complete sync over a fast boot). */
    const char *bs = getenv("ROUTER_SYNC_BUDGET_SECONDS");
    int budget = bs ? atoi(bs) : ROUTER_SYNC_BUDGET_DEFAULT;
    g_sync_deadline = (budget > 0) ? time(NULL) + budget : 0;
    printf("llm-router sync (base %s, budget %ds)\n", base,
           budget > 0 ? budget : 0);
    sync_mcps();
    sync_skills();
    sync_tools();
    fflush(stdout); /* stdout is block-buffered when piped (docker logs) */
    g_sync_deadline = 0; /* later SIGHUP-triggered resyncs re-arm it */
}