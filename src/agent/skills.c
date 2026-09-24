/* Skill index — see skills.h. Pure libc filesystem work, no new deps.
 * The index is built in the parent (skills_init) into static buffers and
 * rebuilt from scratch on SIGHUP resync (skills_init is re-runnable), so
 * every forked worker sees an identical, refreshable catalog. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>

#include "internal.h"
#include "minijson.h"
#include "skills.h"

#define SKILL_MAX 64
#define SKILL_ROOTS_MAX 8
#define SKILL_IDX_TEXT_MAX (SKILL_MAX * (SKILL_NAME_MAX + SKILL_DESC_MAX + 8))

static SkillInfo g_skills[SKILL_MAX];
static int g_nskills = 0;
static char g_index_text[SKILL_IDX_TEXT_MAX + 1];

/* ---- dir discovery -------------------------------------------------- */

/* Profile allow-list: HARNESS_SKILLS_ALLOW="a,b" restricts the catalog to
 * those skill names; unset/empty allows everything. Lets each hosting
 * example (invest.lume, a future media.lume, …) expose only the skills its
 * task actually needs, without moving files in the shared corpus. */
static int skill_allowed(const char *name) {
    const char *allow = getenv("HARNESS_SKILLS_ALLOW");
    if (!allow || !allow[0]) return 1;
    char buf[1024];
    set_str(buf, sizeof buf, allow);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        trim_whitespace(tok);
        if (strcmp(tok, name) == 0) return 1;
    }
    return 0;
}

static void add_root(char roots[][MAX_PATH_SIZE], int *n, const char *path) {
    if (!path || !path[0] || *n >= SKILL_ROOTS_MAX) return;
    /* dedupe */
    for (int i = 0; i < *n; i++) {
        if (strcmp(roots[i], path) == 0) return;
    }
    set_str(roots[*n], MAX_PATH_SIZE, path);
    (*n)++;
    /* parent needs this if it lives behind relative-dir env vars — resolve
     * once here so scanning doesn't depend on the server's cwd later.
     * realpath(path, NULL) returns a malloc'd full path, avoiding a fixed
     * stack buffer: realpath(3)'s _FORTIFY_SOURCE check aborts when the
     * caller's buffer is smaller than PATH_MAX (MAX_PATH_SIZE here is 1024),
     * which Ubuntu's default gcc flags trip unconditionally. */
    char *real = realpath(path, NULL);
    if (real) {
        set_str(roots[*n - 1], MAX_PATH_SIZE, real);
        free(real);
    }
}

/* Frontmatter: ---\nname: x\ndescription: y\n---\n ...
 * Token-minimal (matches the TS regex intent): a field is "key: value"
 * where key is name/description, value the rest of the line, trimmed. */
typedef struct {
    char name[SKILL_NAME_MAX + 1];
    char desc[SKILL_DESC_MAX + 1];
} SkillFm;

static void fm_scan_field(SkillFm *fm, const char *line) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "name:", 5) == 0) {
        p += 5;
        while (*p == ' ' || *p == '\t') p++;
        set_str_utf8(fm->name, sizeof fm->name, p);
    } else if (strncmp(p, "description:", 12) == 0) {
        p += 12;
        while (*p == ' ' || *p == '\t') p++;
        set_str_utf8(fm->desc, sizeof fm->desc, p);
    }
}

static void parse_frontmatter(const char *path, SkillFm *fm) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    int fm_open = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (strncmp(s, "---", 3) == 0) {
            /* first --- opens, second closes */
            if (!fm_open) {
                fm_open = 1;
                continue;
            }
            break;
        }
        if (fm_open) {
            size_t l = strlen(s);
            while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r')) s[--l] = '\0';
            fm_scan_field(fm, s);
        }
    }
    fclose(f);
}

static int add_skill(const char *dir, const char *name) {
    /* name must be a plain directory name: no separators/dots — the model
     * passes skill names around, so curb path tricks before we touch disk. */
    if (!name[0] || strpbrk(name, "/\\")) return 0;
    if (!skill_allowed(name)) return 0;
    for (int i = 0; i < g_nskills; i++) {
        if (strcmp(g_skills[i].name, name) == 0) return 0; /* earlier wins */
    }
    char md[MAX_PATH_SIZE];
    int w = snprintf(md, sizeof md, "%s/%s/SKILL.md", dir, name);
    /* A root deep enough to overflow the buffer must be skipped, not silently
     * indexed under a truncated path (which could name an unrelated file). */
    if (w < 0 || (size_t)w >= sizeof md) return 0;
    /* only index real skills: a collect-layout dir (e.g. skills/router with
     * no top-level SKILL.md) must not leak a phantom entry */
    if (access(md, R_OK) != 0) return 0;
    SkillFm fm;
    memset(&fm, 0, sizeof fm);
    parse_frontmatter(md, &fm);
    if (g_nskills >= SKILL_MAX) return 0;
    SkillInfo *sk = &g_skills[g_nskills];
    set_str_utf8(sk->name, sizeof sk->name, fm.name[0] ? fm.name : name);
    set_str_utf8(sk->desc, sizeof sk->desc, fm.desc);
    set_str(sk->path, sizeof sk->path, md);
    g_nskills++;
    return 1;
}

int skills_init(void) {
    /* Re-runnable (SIGHUP resync calls it again): start from an empty index
     * so catalog refreshes replace the previous one instead of stacking. */
    g_nskills = 0;
    g_index_text[0] = '\0';
    char roots[SKILL_ROOTS_MAX][MAX_PATH_SIZE];
    int n = 0;
    memset(roots, 0, sizeof roots);

    const char *cwd = getcwd(NULL, 0);
    if (cwd) {
        char p[MAX_PATH_SIZE];
        snprintf(p, sizeof p, "%s/skills", cwd);
        add_root(roots, &n, p);
        /* router-synced skills land under skills/router/<name>/SKILL.md
         * (one level deeper than the cwd root); index them explicitly so
         * skill-run can reach the router catalog. */
        snprintf(p, sizeof p, "%s/skills/router", cwd);
        add_root(roots, &n, p);
    }
    add_root(roots, &n, getenv("HARNESS_SKILLS_DIR"));
    /* auto-detect the resolve-skills submodule next to the server.
     * `cwd` is released only after its last use: freeing it inside the block
     * above left a dangling (but still non-NULL) pointer here. GCC's
     * -Wuse-after-free flags it; clang does not. */
    if (cwd) {
        char p[MAX_PATH_SIZE];
        if (snprintf(p, sizeof p, "%s/resolve-skills/skills", cwd) > 0) {
            add_root(roots, &n, p);
        }
        free((void *)cwd);
    }
    const char *extra = getenv("SKILLS_EXTRA_DIRS");
    if (extra) {
        char buf[SKILL_ROOTS_MAX * MAX_PATH_SIZE];
        set_str(buf, sizeof buf, extra);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            trim_whitespace(tok);
            add_root(roots, &n, tok);
        }
    }

    for (int r = 0; r < n; r++) {
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            add_skill(roots[r], e->d_name);
        }
        closedir(d);
    }

    /* render index text */
    size_t off = 0;
    for (int i = 0; i < g_nskills && off < sizeof g_index_text; i++) {
        int w = snprintf(g_index_text + off, sizeof g_index_text - off,
                         "- %s: %s\n", g_skills[i].name, g_skills[i].desc);
        if (w < 0) break;
        off += (size_t)w;
    }
    return g_nskills;
}

int skills_count(void) {
    return g_nskills;
}

const SkillInfo *skills_get(int i) {
    if (i < 0 || i >= g_nskills) return NULL;
    return &g_skills[i];
}

const char *skills_index_text(void) {
    return g_index_text;
}

char *skills_read(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_nskills; i++) {
        if (strcmp(g_skills[i].name, name) == 0) {
            FILE *f = fopen(g_skills[i].path, "r");
            if (!f) {
                fprintf(stderr, "[skills] read %s: %s\n", g_skills[i].path,
                        strerror(errno));
                return NULL;
            }
            sbuf b = {0};
            char chunk[4096];
            size_t r;
            while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
                sb_mem(&b, chunk, r);
                if (b.oom) break;
            }
            fclose(f);
            return b.p;
        }
    }
    return NULL;
}