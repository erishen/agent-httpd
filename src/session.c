/* Memory: session persistence + fact store — see session.h.
 * JSON files live under .data/sessions/ (atomically written via tmp+rename).
 * Parsing rides minijson's tolerant reader; rendering rides sbuf. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>

#include "internal.h"
#include "minijson.h"
#include "session.h"

#define DATA_DIR ".data"
#define SESS_DIR ".data/sessions"

static char *session_path(const char *id, char *out, size_t sz) {
    if (!id || !*id) {
        snprintf(out, sz, "%s/memory.json", DATA_DIR);
    } else {
        snprintf(out, sz, "%s/%s.json", SESS_DIR, id);
    }
    return out;
}

/* Session IDs are opaque identifiers minted by session_id_new (hex-only).
 * Anything else arriving in a request body is suspect: '.'/'/' characters
*  could turn the id into a path traversal against the sessions dir.
 *  Players that can read/write/remember never see a raw client value unless
 *  it passes this check. The rule is a path-safety allowlist (alnum, dash,
 *  underscore — plus "+" as used by dev-server's echo tool), not hex-only:
 *  the UI mints "s-<random>" ids and tests use short ids like "s1", which
 *  must keep working. Traversal needs '/', '\', '..' or control bytes, and
 *  none of those is allowed here. */
int session_id_valid(const char *id) {
    if (!id || !*id) return 0;
    size_t n = strnlen(id, SESSION_ID_MAX + 1);
    if (n > SESSION_ID_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '+')) return 0;
    }
    if (n == 1 && id[0] == '.') return 0; /* explicit: no dot allowed at all */
    return 1;
}

static void mk_data_dirs(void) {
    mkdir(DATA_DIR, 0700);
    mkdir(SESS_DIR, 0700);
}

void session_id_new(char *out, size_t sz) {
    static const char HEX[] = "0123456789abcdef";
    unsigned long r;
    FILE *u = fopen("/dev/urandom", "r");
    if (u) {
        if (fread(&r, sizeof r, 1, u) != 1) r = (unsigned)time(NULL);
        fclose(u);
    } else {
        r = ((unsigned)time(NULL) << 16) ^ (unsigned)getpid() ^ (unsigned)rand();
    }
    r ^= (unsigned long)(uintptr_t)&r;
    char buf[32];
    snprintf(buf, sizeof buf, "%08lx%08lx", r, (r >> 1) ^ (unsigned long)time(NULL));
    size_t j = 0;
    for (size_t i = 0; i < strnlen(buf, sizeof buf) && j < sz - 1 && j < SESSION_ID_MAX; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') out[j++] = buf[i];
        else if (buf[i] >= 'a' && buf[i] <= 'f') out[j++] = buf[i];
        else out[j++] = HEX[((unsigned)(unsigned char)buf[i]) & 0xf];
    }
    out[j] = '\0';
}

int session_load(const char *id, Session *s) {
    memset(s, 0, sizeof *s);
    if (id) set_str(s->id, sizeof s->id, id);
    /* non-empty ids are opaque hex tokens; reject anything else before it
     * can reach a path (defense in depth on top of the llm.c gate) */
    if (id && id[0] && !session_id_valid(id)) return -1;
    char path[MAX_PATH_SIZE];
    session_path(id, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 1 : -1;

    sbuf b = {0};
    char chunk[4096];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
        sb_mem(&b, chunk, r);
        if (b.oom) break;
    }
    fclose(f);
    int rc = 0;
    const char *p = b.p ? b.p : "";
    if (*p == '{' && !b.oom) {
        p++;
        for (;;) {
            p = jws(p);
            if (*p == '}') break;
            char key[24];
            if (!jread_string(&p, key, sizeof key)) { rc = -1; break; }
            p = jws(p);
            if (*p != ':') { rc = -1; break; }
            p = jws(p + 1);
            if (strcmp(key, "messages") == 0 && *p == '[') {
                p++;
                for (;;) {
                    p = jws(p);
                    if (*p == ']') { p++; break; }
                    if (*p == ',') { p++; continue; }
                    if (*p != '{') { rc = -1; goto done; }
                    p++;
                    char role[16] = "", content[SESSION_CONTENT_MAX + 1] = "";
                    for (;;) {
                        p = jws(p);
                        if (*p == '}') { p++; break; }
                        char mk[24];
                        if (!jread_string(&p, mk, sizeof mk)) { rc = -1; goto done; }
                        p = jws(p);
                        if (*p != ':') { rc = -1; goto done; }
                        p = jws(p + 1);
                        if (strcmp(mk, "role") == 0) {
                            if (!jread_string(&p, role, sizeof role)) { rc = -1; goto done; }
                        } else if (strcmp(mk, "content") == 0) {
                            if (!jread_string(&p, content, sizeof content)) { rc = -1; goto done; }
                        } else if (jskip_value(&p) != 0) { rc = -1; goto done; }
                        p = jws(p);
                        if (*p == ',') p++;
                    }
                    if (s->n_msgs < SESSION_MSG_MAX &&
                        (strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0)) {
                        set_str(s->msgs[s->n_msgs].role, sizeof s->msgs[0].role, role);
                        set_str(s->msgs[s->n_msgs].content,
                                sizeof s->msgs[0].content,
                                content[0] ? content : "(no text)");
                        s->n_msgs++;
                    }
                    p = jws(p);
                    if (*p == ',') p++;
                }
            } else if (strcmp(key, "facts") == 0 && *p == '{') {
                p++;
                for (;;) {
                    p = jws(p);
                    if (*p == '}') { p++; break; }
                    if (*p == ',') { p++; continue; }
                    char fk[SESSION_KEY_MAX + 1];
                    if (!jread_string(&p, fk, sizeof fk)) { rc = -1; goto done; }
                    p = jws(p);
                    if (*p != ':') { rc = -1; goto done; }
                    p = jws(p + 1);
                    char fv[SESSION_VAL_MAX + 1];
                    if (!jread_string(&p, fv, sizeof fv)) { rc = -1; goto done; }
                    if (s->n_facts < SESSION_FACTS_MAX && fk[0] && fv[0]) {
                        set_str(s->facts[s->n_facts].key, sizeof s->facts[0].key, fk);
                        set_str(s->facts[s->n_facts].val, sizeof s->facts[0].val, fv);
                        s->n_facts++;
                    }
                    p = jws(p);
                    if (*p == ',') p++;
                }
            } else if (jskip_value(&p) != 0) { rc = -1; goto done; }
            p = jws(p);
            if (*p == ',') {
                p++;
            } else if (*p == '}') {
                break;
            } else {
                rc = -1;
                goto done;
            }
        }
    }
done:
    free(b.p);
    return rc;
}

void session_prune_old(double max_age_days) {
    /* 启动时清理超过 max_age_days 的会话文件(memory.json 等非 session 文件
     * 不动)。目录不存在或校验失败按 0 处理,调用方不需为此失败。 */
    time_t now = time(NULL);
    DIR *d = opendir(SESS_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        size_t l = strlen(name);
        /* need at least 5 chars for an "x.json" name; the shorter guard
         * also prevents `name + l - 5` (the .json suffix test below) from
         * reading bytes before `name`. */
        if (l < 5 || l > SESSION_ID_MAX + 5) continue;
        if (strcmp(name, "memory.json") == 0) continue;
        if (strcmp(name + l - 5, ".json") != 0) continue;
        char p[MAX_PATH_SIZE];
        snprintf(p, sizeof p, "%s/%s", SESS_DIR, name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (difftime(now, st.st_mtime) > max_age_days * 86400) {
            unlink(p);
        }
    }
    closedir(d);
}

int session_save(const Session *s) {
    if (s->id[0] && !session_id_valid(s->id)) return -1;
    mk_data_dirs();
    sbuf b = {0};
    sb_str(&b, "{\"id\":");
    sb_json_str(&b, s->id);
    sb_str(&b, ",\"messages\":[");
    for (int i = 0; i < s->n_msgs; i++) {
        if (i) sb_chr(&b, ',');
        sb_str(&b, "{\"role\":");
        sb_json_str(&b, s->msgs[i].role);
        sb_str(&b, ",\"content\":");
        sb_json_str(&b, s->msgs[i].content);
        sb_chr(&b, '}');
    }
    sb_str(&b, "],\"facts\":{");
    for (int i = 0; i < s->n_facts; i++) {
        if (i) sb_chr(&b, ',');
        sb_json_str(&b, s->facts[i].key);
        sb_chr(&b, ':');
        sb_json_str(&b, s->facts[i].val);
    }
    sb_str(&b, "}}\n");
    if (b.oom) {
        free(b.p);
        return -1;
    }

    char path[MAX_PATH_SIZE], tmp[MAX_PATH_SIZE + 16];
    session_path(s->id, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(b.p);
        return -1;
    }
    int ok = fwrite(b.p, 1, b.len, f) == b.len;
    if (fflush(f) != 0) ok = 0;
    if (ok) {
        /* Sessions carry conversation transcripts (potential PII); the
         * tmp file's mode survives the rename below. */
        if (fchmod(fileno(f), 0600) != 0) ok = 0;
    }
    if (fclose(f) != 0) ok = 0;
    if (ok && rename(tmp, path) != 0) ok = 0;
    free(b.p);
    if (!ok) {
        unlink(tmp);
        fprintf(stderr, "[session] save %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

void session_append(Session *s, const char *role, const char *content) {
    if (s->n_msgs >= SESSION_MSG_MAX) {
        memmove(s->msgs, s->msgs + 1, (size_t)(SESSION_MSG_MAX - 1) * sizeof s->msgs[0]);
        s->n_msgs--;
    }
    set_str(s->msgs[s->n_msgs].role, sizeof s->msgs[0].role, role);
    set_str(s->msgs[s->n_msgs].content, sizeof s->msgs[0].content, content);
    s->n_msgs++;
}

int session_fact_set(Session *s, const char *key, const char *val) {
    if (!key || !key[0] || strpbrk(key, "{}:\",\n\r")) return -1;
    char v[SESSION_VAL_MAX + 1];
    set_str(v, sizeof v, val ? val : "");
    for (int i = 0; i < s->n_facts; i++) {
        if (strcmp(s->facts[i].key, key) == 0) {
            set_str(s->facts[i].val, sizeof s->facts[0].val, v);
            return 0;
        }
    }
    if (s->n_facts >= SESSION_FACTS_MAX) return -1;
    set_str(s->facts[s->n_facts].key, sizeof s->facts[0].key, key);
    set_str(s->facts[s->n_facts].val, sizeof s->facts[0].val, v);
    s->n_facts++;
    return 0;
}

const char *session_fact_get(const Session *s, const char *key) {
    if (!key) return NULL;
    for (int i = 0; i < s->n_facts; i++) {
        if (strcmp(s->facts[i].key, key) == 0) return s->facts[i].val;
    }
    return NULL;
}

int session_fact_set_file(const char *id, const char *key, const char *val) {
    Session s;
    if (session_load(id, &s) < 0) return -1;
    if (session_fact_set(&s, key, val) != 0) return -1;
    return session_save(&s);
}

int session_fact_get_file(const char *id, const char *key,
                          char *out, size_t outsz) {
    Session s;
    if (session_load(id, &s) != 0) return -1;
    const char *v = session_fact_get(&s, key);
    if (!v) return -1;
    set_str(out, outsz, v);
    return 0;
}

void session_render_extra(const Session *s, sbuf *out) {
    const char *scope = s->id[0] ? s->id : "global";
    sb_str(out, "[session memory ");
    sb_str(out, scope);
    sb_str(out, "]");
    if (s->n_facts) {
        sb_str(out, "\nFacts:");
        for (int i = 0; i < s->n_facts; i++) {
            sb_chr(out, '\n');
            sb_str(out, "- ");
            sb_str(out, s->facts[i].key);
            sb_str(out, ": ");
            sb_str(out, s->facts[i].val);
        }
    }
    if (s->n_msgs) {
        sb_str(out, "\nRecent exchange:");
        for (int i = 0; i < s->n_msgs; i++) {
            sb_chr(out, '\n');
            sb_str(out, s->msgs[i].role);
            sb_str(out, ": ");
            sb_str(out, s->msgs[i].content);
        }
    }
}