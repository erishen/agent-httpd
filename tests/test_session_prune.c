/* Unit test for the session prune contract (P1 follow-up: session prune vs
 * worker writes). session_save() writes tmp+rename (atomic), and the prune
 * only matches "x.json" names, so a worker mid-write (its `.tmp.<pid>`
 * file) can never be unlinked under it. The one real rule to pin down:
 * prune removes only stale `*.json` files, never memory.json, non-JSON
 * files, or the tmp files a concurrent save is writing. This test builds
 * a scratch .data/ tree, ages one file past the 30-day TTL, and asserts
 * the prune leaves everything else alone. POSIX only (utimes/dirent), so
 * it runs in the Linux/macOS CI legs; the regression main is also the
 * local non-sanitizer path. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "httpd.h"
#include "session.h"

/* stubs — bounded setters mirroring src/core/util.c (session.c links them
 * in the real server; the test compiles the session TU alone). */
void set_str(char *dst, size_t dst_size, const char *src) {
    if (!dst_size) return;
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void set_str_utf8(char *dst, size_t dst_size, const char *src) {
    set_str(dst, dst_size, src);
}

static int failures = 0;
static void expect(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

static void mkfile(const char *p, time_t mtime) {
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs("{}", f);
    fclose(f);
    if (mtime) {
        struct timeval tv[2];
        tv[0].tv_sec = mtime; tv[0].tv_usec = 0;
        tv[1].tv_sec = mtime; tv[1].tv_usec = 0;
        if (utimes(p, tv) != 0) { perror("utimes"); exit(2); }
    }
}

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void rmrf(const char *p) {
    DIR *d = opendir(p);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char c[MAX_PATH_SIZE];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        struct stat st;
        if (stat(c, &st) == 0 && S_ISDIR(st.st_mode)) rmrf(c);
        else unlink(c);
    }
    closedir(d);
    rmdir(p);
}

int main(void) {
    char tmpl[] = "/tmp/sessprune.XXXXXX"; /* mutable: mkdtemp writes into it */
    char *d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); return 2; }
    if (chdir(d) != 0) { perror("chdir"); return 2; } /* glibc: warn_unused_result */
    if (mkdir(".data", 0700) != 0 && errno != EEXIST) { perror("mkdir .data"); return 2; }
    if (mkdir(".data/sessions", 0700) != 0 && errno != EEXIST) { perror("mkdir sessions"); return 2; }

    time_t now = time(NULL);
    time_t old = now - 40 * 86400; /* past the 30-day TTL */

    mkfile(".data/sessions/old.json", old);              /* pruned */
    mkfile(".data/sessions/new.json", now);              /* kept */
    mkfile(".data/sessions/sess.json.tmp.999", old);     /* worker mid-write tmp: kept */
    mkfile(".data/memory.json", old);                    /* protected: kept */
    mkfile(".data/sessions/note.txt", old);              /* non-JSON: kept */

    session_prune_old(30);

    expect(!exists(".data/sessions/old.json"), "stale session pruned");
    expect(exists(".data/sessions/new.json"), "fresh session kept");
    expect(exists(".data/sessions/sess.json.tmp.999"), "worker tmp untouched by prune");
    expect(exists(".data/memory.json"), "memory.json untouched");
    expect(exists(".data/sessions/note.txt"), "non-JSON file untouched");

    rmrf(d);
    if (failures == 0)
        printf("ok test_session_prune: atomic tmp+rename contract holds (5 cases)\n");
    return failures ? 1 : 0;
}
