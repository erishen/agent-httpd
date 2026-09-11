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

static const RouterMcpTemplate k_known_mcps[] = {
    {"fs", "npx", "-y @modelcontextprotocol/server-filesystem $FSROOT"},
    {"think", "npx", "-y @modelcontextprotocol/server-sequential-thinking"},
    {"memory", "npx", "-y @modelcontextprotocol/server-memory"},
};
#define KNOWN_MCPS (sizeof k_known_mcps / sizeof k_known_mcps[0])

static const char *router_base(void) {
    const char *u = getenv("ROUTER_API_URL");
    if (u && *u) return u;
    /* Default: the same gateway the agent already talks to. A direct
     * provider URL simply won't answer /v1/mcps like a router -> no-op. */
    return getenv("LLM_API_URL");
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
        execlp("curl", "curl", "-sS", "-L", "--max-time", mt,
               "--proto", "=http,https", "-H", auth, url, (char *)NULL);
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
    *have = 1;
    return b.p;
}

/* Parse `{"skills":[{"name":..,"description":..}]}` into a flat array of
 * names. Returns malloc'd array of malloc'd names, count in *out. */
static char **parse_skill_names(const char *json, int *out) {
    *out = 0;
    const char *sk = jfind_value(json, "skills");
    if (!sk || *sk != '[') return NULL;
    const char *p = sk + 1;
    char **names = NULL;
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

        const char *nv = jfind_value(obj, "name");
        if (nv && *nv == '"') {
            char name[256];
            const char *np = nv;
            if (!jread_string(&np, name, sizeof name)) continue;
            char **grown = realloc(names, sizeof(char *) * (size_t)(n + 1));
            if (!grown) break;
            names = grown;
            names[n] = strdup(name);
            if (!names[n]) break;
            n++;
        }
    }
    *out = n;
    return names;
}

/* Write one skill body to skills/router/<name>/SKILL.md. `body` is the raw
 * JSON string value (opening quote included); unescaped once here. */
static void materialize_skill(const char *name, const char *body) {
    if (!name || !*name || !body || !*body) return;
    /* name is dir-safe by construction (skills.c rejects '/' and '\\'),
     * and we only write into the skills/ subtree. */
    char dir[ROUTER_PATH_MAX];
    if (snprintf(dir, sizeof dir, "skills/router/%s", name) >= (int)sizeof dir) return;
    if (mkdir("skills/router", 0755) != 0 && errno != EEXIST) return;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return;

    char path[ROUTER_PATH_MAX];
    if (snprintf(path, sizeof path, "%s/SKILL.md", dir) >= (int)sizeof path) return;
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[router] cannot write skill %s: %s\n", path, strerror(errno));
        return;
    }
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
                if (snprintf(args, sizeof args, "-y @modelcontextprotocol/server-filesystem %s",
                             root) >= (int)sizeof args) continue;
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
    char **names = NULL;
    int n = 0;
    names = parse_skill_names(json, &n);
    free(json);
    if (!names) return;

    int ok = 0;
    for (int i = 0; i < n && names[i]; i++) {
        char ep[ROUTER_PATH_MAX];
        /* names are alnum/dash only, no URL-encoding needed */
        if (snprintf(ep, sizeof ep, "skills/%s", names[i]) >= (int)sizeof ep) continue;
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
                materialize_skill(names[i], bv);
            } else {
                fprintf(stderr, "[router] skill '%s' body not a string, skip\n", names[i]);
            }
        }
        free(body);
        ok++;
    }
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
    printf("llm-router skills: %d skill(s) synced -> skills/router/\n", ok);
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
    g_sync_deadline = 0; /* later SIGHUP-triggered resyncs re-arm it */
}