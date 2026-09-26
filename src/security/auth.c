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
#include <time.h>

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

/* ---- 登出 / 切换账号支持（AUTH_REALM_FILE）----
 * 浏览器 Basic Auth 凭据按 origin 缓存且基本不看 realm：登出后浏览器会静默
 * 重发旧凭据，旧账号仍有效 → 直接 200，永远切不了用户。realm 轮换（计数 N
 * 使 401 挑战变 "<realm>#N"）只在 401 发生时才有意义 —— 所以核心是 auth 门
 * 的登出拒绝记录：/logout 把「刚登出用户名|时间戳」写入 AUTH_REALM_FILE 第 2 行，
 * 该用户 30 秒内的请求被拒（消费式：只拦一次即清除，30s 后自动放行，用户
 * 可以重新登录）。计数与记录都落文件而非进程内存，prefork 各进程天然一致。 */
static char g_realm_file[MAX_PATH_SIZE] = "";
static char g_realm_rotated[128] = "";

static void realm_file_lazy_init(void) {
    const char *e = getenv("AUTH_REALM_FILE");
    if (e && e[0]) snprintf(g_realm_file, sizeof(g_realm_file), "%s", e);
}

static int realm_file_read(int *out_n) {
    FILE *f = fopen(g_realm_file, "r");
    if (!f) { *out_n = 0; return -1; }
    int n = 0;
    if (fscanf(f, "%d", &n) != 1) n = 0;
    *out_n = n;
    fclose(f);
    return 0;
}

/* 当前 401 挑战应使用的 realm：计数 N>0 时返回 "<realm>#N"，否则原值。 */
const char *auth_realm_current(void) {
    static int inited = 0;
    if (!inited) { inited = 1; realm_file_lazy_init(); }
    int n = 0;
    if (!g_realm_file[0] || realm_file_read(&n) != 0 || n <= 0)
        return g_auth_realm;
    snprintf(g_realm_rotated, sizeof(g_realm_rotated), "%s#%d",
             g_auth_realm, n);
    return g_realm_rotated;
}

/* 计数 +1 落盘；user 非空时同时写第 2 行「user|epoch」登出记录（该用户 30s 内
 * 的登录被 check_basic_auth 拒绝，浏览器被迫弹框而不是静默重登旧账号）。
 * 未配置 env 或不可写 → -1。 */
int auth_logout_realm_bump(const char *user) {
    static int inited = 0;
    if (!inited) { inited = 1; realm_file_lazy_init(); }
    if (!g_realm_file[0]) return -1;
    int n = 0;
    if (realm_file_read(&n) == 0) n++;
    else n = 1;
    FILE *w = fopen(g_realm_file, "w");
    if (!w) return -1;
    fprintf(w, "%d\n", n);
    if (user && user[0])
        fprintf(w, "%s|%ld\n", user, (long)time(NULL));
    fclose(w);
    return n;
}

/* 用户是否处于 30s 登出拒绝窗口内（AUTH_REALM_FILE 第 2 行记录）。 */
/* 登出记录的一次性消费：AUTH_REALM_FILE 第 2 行 "user|epoch" 与 user 匹配
 * 且未过期(<30s)时清除该记录并返回 1 —— 调用方应拒绝本次请求（401），
 * 使浏览器的缓存凭据被拒一次、登录框重新弹出。已过期记录静默清除。
 * 只拦一次：之后手动重新登录同一账号立即放行（不会卡死）。
 * prefork 各 worker 共享同一份文件；并发消费最多多一个 401，无害。 */
static int auth_logout_consume(const char *user) {
    static int inited = 0;
    if (!inited) { inited = 1; realm_file_lazy_init(); }
    if (!g_realm_file[0] || !user || !user[0]) return 0;
    FILE *f = fopen(g_realm_file, "r");
    if (!f) return 0;
    char line1[32] = "", line2[160] = "";
    int has1 = fgets(line1, sizeof(line1), f) != NULL; /* line 1: counter */
    int has2 = fgets(line2, sizeof(line2), f) != NULL; /* line 2: user|epoch */
    fclose(f);
    if (!has2) return 0;
    char lu[64] = "";
    long ts = 0;
    if (sscanf(line2, "%63[^|]|%ld", lu, &ts) != 2 || strcmp(lu, user) != 0)
        return 0;
    /* 清除第 2 行（保留计数）：过期或新鲜都清除，但只拦截新鲜的 */
    int n = 0;
    if (has1) sscanf(line1, "%d", &n);
    FILE *w = fopen(g_realm_file, "w");
    if (w) { fprintf(w, "%d\n", n); fclose(w); }
    return (long)time(NULL) - ts < 30; /* 新鲜 → 拦这次；过期 → 放行 */
}
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

/* /logout 路径识别：登出端点必须豁免 Basic Auth（见 auth.c 文件头注释：
 * 旧凭据场景下 401 门会把它挡掉，无法登出切账号）。path 可能带 query。 */
int is_logout_path(const char *path) {
    const char *p = path;
    while (*p && *p != '?') p++;
    return (int)(p - path) == 7 && strncmp(path, "/logout", 7) == 0;
}

int check_basic_auth(const char *header_value) {
    if (!g_auth_file[0]) return 1; /* auth disabled */
    if (!header_value || strncasecmp(header_value, "Basic ", 6) != 0) {
        return 0;
    }
    char creds[256 + 128 + 2];
    b64_decode(header_value + 6, creds, sizeof(creds));
    char *colon = strchr(creds, ':');
    if (!colon) return 0;
    *colon = '\0';
    const char *pass = colon + 1;
    /* 登出拒绝窗口：该用户刚 /logout（30s 内、一次性）→ 拒绝本次，使浏览器的
     * 缓存凭据被 401 一次、登录框重新弹出（realm 已轮换，旧桶失效）。 */
    if (auth_logout_consume(creds))
        return 0;
    for (int i = 0; i < g_htpasswd_count; i++) {
        if (strcmp(creds, g_htpasswd[i].user) == 0) {
            return secret_matches(pass, g_htpasswd[i].secret);
        }
    }
    return 0;
}
