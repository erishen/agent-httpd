/* ---- Basic Auth (RFC 7617) ----
 * htpasswd file: lines of "user:secret". Only strong crypt(3) hashes are
 * accepted: $5$ (SHA-256), $6$ (SHA-512) or bcrypt ($2a$/$2b$/$2y$).
 * Plaintext secrets and weak schemes (DES 13-char, $1$ MD5, $apr1$) are
 * rejected at load time and never match - a weak file can never become
 * "allow all". AGENTHTTPD_ALLOW_WEAK_AUTH=1 is the explicit local-dev
 * escape hatch that restores the old tolerant behavior (and warns loudly).
 * Malformed lines are always skipped; empty g_auth_file disables auth
 * entirely. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#elif defined(HAVE_CRYPT)
/* macOS: crypt(3) lives in libc and is declared via <unistd.h>, but there
 * is no <crypt.h>; keep a prototype for non-glibc platforms without one. */
extern char *crypt(const char *key, const char *setting);
#endif

#include "internal.h"
#include "bcrypt.h"

char g_auth_file[MAX_PATH_SIZE] = "";
char g_auth_realm[128] = DEFAULT_AUTH_REALM;
/* Basic Auth 最近一次校验通过的用户名（仅 g_auth_file 非空时由 check_basic_auth
 * 写入；校验失败/认证关闭时清空）。供 lumed DSL 的 req map 读取，实现
 * "按账号区分内容"。进程内单请求处理模型下安全：每个 worker 串行处理请求。 */
char g_auth_user[64] = "";
#define HTPASSWD_MAX_ENTRIES 64

struct htpasswd_entry {
    char user[64];
    char secret[256];
};

static struct htpasswd_entry g_htpasswd[HTPASSWD_MAX_ENTRIES];
static int g_htpasswd_count = 0;

void b64_decode(const char *in, char *out, size_t out_size) {
    static int8_t T[256];
    static int initialized = 0;
    if (!initialized) {
        /* Anything outside the base64 alphabet maps to -1 (sentinel): a
         * malformed Authorization header then stops decoding instead of
         * being silently rewritten into 'A' bytes (which could wrongfully
         * authenticate). */
        for (int i = 0; i < 256; i++) T[i] = -1;
        static const char alpha[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) T[(unsigned char)alpha[i]] = (int8_t)i;
        initialized = 1;
    }
    size_t o = 0;
    unsigned acc = 0; /* unsigned: shifting a signed int by 6 is UB once
                       * accumulated bits exceed INT_MAX (UBSan catches it) */
    int bits = 0;
    for (; *in && *in != '=' && o + 1 < out_size; in++) {
        int8_t v = T[(unsigned char)*in];
        if (v < 0) break;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (char)((acc >> bits) & 0xff);
        }
    }
    out[o] = '\0';
}

/* Constant-time string equality: never short-circuits on the first
 * differing byte, so it leaks neither the position of a mismatch nor
 * (beyond an equality bit) the lengths. Used for credential comparison. */
static int ct_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = (unsigned)(la ^ lb);
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < la && i < lb; i++) {
        diff |= (unsigned)(pa[i] ^ pb[i]);
    }
    return diff == 0;
}

/* Strong hash formats only: crypt(3) SHA-2 ($5$/$6$) or bcrypt. Everything
 * else (plaintext, 13-char DES, $1$ MD5, $apr1$) is weak and only accepted
 * under the AGENTHTTPD_ALLOW_WEAK_AUTH dev escape hatch. */
static int is_strong_hash(const char *s) {
    return strncmp(s, "$5$", 3) == 0 ||
           strncmp(s, "$6$", 3) == 0 ||
           strncmp(s, "$2a$", 4) == 0 ||
           strncmp(s, "$2b$", 4) == 0 ||
           strncmp(s, "$2y$", 4) == 0;
}

static int secret_matches(const char *supplied, const char *stored) {
    int stored_is_hash = (stored[0] == '$') ||
                         (strlen(stored) == 13 && strspn(stored, "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") == 13);
    if (!stored_is_hash) {
        return ct_eq(supplied, stored); /* plaintext (incl. {SHA}-style) */
    }
    /* bcrypt ($2a$/$2b$/$2y$/$2x$): macOS/BSD libc crypt(3) only implements
     * legacy DES (and glibc crypt lacks bcrypt too), so verify with the
     * bundled portable implementation. Other hash schemes keep the crypt(3)
     * path ($5$/$6$ on glibc). */
    if (bcrypt_is_hash(stored)) {
        return bcrypt_verify(supplied, stored);
    }
#ifdef HAVE_CRYPT
    const char *got = crypt(supplied, stored);
    if (!got) return 0; /* unsupported scheme on this platform: deny */
    return ct_eq(got, stored);
#else
    (void)supplied;
    return 0; /* crypt unavailable: hash entries can never match */
#endif
}

int load_htpasswd(const char *path) {
    FILE *f = fopen(path, "r");
    char line[512];
    /* Weak secrets (plaintext / DES / MD5-crypt) are stored only under the
     * explicit dev escape hatch; without it they are skipped with a warning,
     * forcing the file onto strong crypt hashes. Skipped entries never match,
     * so a weak file errs toward "deny", never "allow". */
    int weak_allowed = getenv("AGENTHTTPD_ALLOW_WEAK_AUTH") != NULL;
    if (weak_allowed) {
        fprintf(stderr, "htpasswd: AGENTHTTPD_ALLOW_WEAK_AUTH is set - "
                "plaintext/DES/MD5 secrets are accepted. DEV USE ONLY, "
                "never in production.\n");
    }
    if (!f) {
        fprintf(stderr, "cannot open htpasswd file: %s\n", path);
        return -1;
    }
    g_htpasswd_count = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        char *colon = strchr(line, ':');
        if (!colon || colon == line ||
            (size_t)(colon - line) >= sizeof(g_htpasswd[0].user) ||
            strlen(colon + 1) >= sizeof(g_htpasswd[0].secret) ||
            g_htpasswd_count >= HTPASSWD_MAX_ENTRIES) {
            fprintf(stderr, "htpasswd: skipping malformed line in %s\n", path);
            continue;
        }
        *colon = '\0';
        if (!is_strong_hash(colon + 1) && !weak_allowed) {
            fprintf(stderr,
                    "htpasswd: '%s' in %s has a plaintext or weak secret - "
                    "only crypt(3) $5$/$6$ (SHA-2) or bcrypt ($2a$/$2b$/$2y$) "
                    "hashes are accepted (e.g. `openssl passwd -6` or "
                    "`htpasswd -B`); entry skipped. Local-dev escape hatch: "
                    "AGENTHTTPD_ALLOW_WEAK_AUTH=1\n",
                    line, path);
            continue;
        }
        strcpy(g_htpasswd[g_htpasswd_count].user, line);
        strcpy(g_htpasswd[g_htpasswd_count].secret, colon + 1);
        g_htpasswd_count++;
    }
    fclose(f);
    if (g_htpasswd_count == 0) {
        fprintf(stderr, "htpasswd: no valid entries in %s\n", path);
        return -1;
    }
    return 0;
}

int check_basic_auth(const char *header_value) {
    if (!g_auth_file[0]) { g_auth_user[0] = '\0'; return 1; } /* auth disabled */
    if (!header_value || strncasecmp(header_value, "Basic ", 6) != 0) {
        g_auth_user[0] = '\0';
        return 0;
    }
    char creds[256 + 128 + 2];
    b64_decode(header_value + 6, creds, sizeof(creds));
    char *colon = strchr(creds, ':');
    if (!colon) { g_auth_user[0] = '\0'; return 0; }
    *colon = '\0';
    const char *pass = colon + 1;
    for (int i = 0; i < g_htpasswd_count; i++) {
        if (strcmp(creds, g_htpasswd[i].user) == 0) {
            int ok = secret_matches(pass, g_htpasswd[i].secret);
            /* 校验通过才记录用户名；失败不保留上次值（防跨请求串用）。 */
            g_auth_user[0] = '\0';
            if (ok) snprintf(g_auth_user, sizeof(g_auth_user), "%s", creds);
            return ok;
        }
    }
    g_auth_user[0] = '\0';
    return 0;
}
