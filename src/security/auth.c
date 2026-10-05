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
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
/* flock()/LOCK_EX: on glibc this lives in <sys/file.h>, which nothing else
 * includes here. Darwin happens to pull it in transitively, so the omission
 * only surfaces as -Werror=implicit-function-declaration on Linux. */
#include <sys/file.h>

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
/* 热重载：记录上次 load 的文件 mtime（纳秒级）；管理端在线增删账号后重写
 * htpasswd，下一个请求在 check_basic_auth 入口检测到 mtime 变化即重新 load。
 * 秒级比较在"同秒内连续写"时会漏触发（管理端快速重置密码/增删账号），
 * 必须比较纳秒：Linux 用 st_mtim，macOS/BSD 用 st_mtimespec。 */
static struct timespec g_htpasswd_mtime = {0, 0};

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
    /* Platform capability probe: $5$/$6$ entries verify through libcrypt,
     * which macOS/BSD only implements as legacy DES — there those entries
     * reject valid credentials *silently* (fail-closed, but confusing: the
     * admin sees 401s for a correct password). Probe once at load time and
     * say so loudly. bcrypt entries are unaffected: the bundled verifier is
     * portable across platforms. */
    int has_crypt_sha = 0;
    for (int i = 0; i < g_htpasswd_count && !has_crypt_sha; i++)
        has_crypt_sha = (strncmp(g_htpasswd[i].secret, "$5$", 3) == 0 ||
                         strncmp(g_htpasswd[i].secret, "$6$", 3) == 0);
    if (has_crypt_sha) {
#ifdef HAVE_CRYPT
        const char *probe = crypt("probe", "$6$probeSalt1");
        if (!probe || strncmp(probe, "$6$", 3) != 0)
            fprintf(stderr,
                    "htpasswd: this platform's crypt(3) cannot verify $6$ "
                    "hashes (macOS/BSD libcrypt is DES-only) - $5$/$6$ "
                    "entries will always be DENIED here. Use bcrypt instead: "
                    "`htpasswd -bnB user pass` (portable, bundled verifier).\n");
#else
        fprintf(stderr,
                "htpasswd: built without crypt(3) - $5$/$6$ entries will "
                "always be DENIED; use bcrypt ($2a$/$2b$/$2y$).\n");
#endif
    }
    struct stat st;
    g_htpasswd_mtime.tv_sec = 0;
    g_htpasswd_mtime.tv_nsec = 0;
    if (stat(path, &st) == 0) {
#ifdef __APPLE__
        g_htpasswd_mtime = st.st_mtimespec;
#else
        g_htpasswd_mtime = st.st_mtim;
#endif
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

/* AUTH_PUBLIC_PATHS（env，";" 分隔）公共路径豁免：这些路径无敏感数据
 * （账号切换页 + 凭据校验端点），必须无认证可达 —— 否则登出后切账号页
 * 本身弹框，形成死锁。段边界前缀匹配："/accounts" 命中 "/accounts" 与
 * "/accounts/list"，不命中 "/accounting"（下一字符须为 '\0' 或 '/'）。
 * 与 is_logout_path 同构，由 event.c / http.c 的 auth 门并列调用。 */
static char g_public_paths[MAX_PATH_SIZE * 2] = "";

static void public_paths_lazy_init(void) {
    const char *e = getenv("AUTH_PUBLIC_PATHS");
    if (e && e[0])
        snprintf(g_public_paths, sizeof(g_public_paths), "%s", e);
}

int is_public_path(const char *path) {
    static int inited = 0;
    if (!inited) { inited = 1; public_paths_lazy_init(); }
    if (!g_public_paths[0] || !path) return 0;
    const char *p = path;
    while (*p && *p != '?') p++; /* 只比到 '?' 为止，支持带 query */
    for (const char *s = g_public_paths; *s; ) {
        const char *sep = strchr(s, ';');
        size_t len = sep ? (size_t)(sep - s) : strlen(s);
        size_t plen = (size_t)(p - path);
        /* 段边界取在条目匹配结束处 path[len]：为 '\0'（该条目即整条路径，
         * 如 "/" 精确匹配根）、'/'（其子路径）或 '?'（同一路径带 query）。
         * 用 plen 做位置会让 "/" 条目误放行一切（strncmp 前缀恒真 + 路径
         * 末字符恒 '\0'），必须用 len。 */
        char next = (len < MAX_PATH_SIZE) ? path[len] : '\0';
        if (len > 0 && plen >= len && strncmp(s, path, len) == 0 &&
            (next == '\0' || next == '/' || next == '?'))
            return 1;
        s += (sep ? (size_t)(sep - s) : len) + 1;
    }
    return 0;
}

/* htpasswd 热重载：mtime 变化（管理端在线增删账号）时重新加载。
 * 文件暂不可读时 load 失败会保留旧表（fopen 失败先 return，count 未清空），
 * 不会把认证门打成"全拒"或"全放"。 */
static void htpasswd_reload_if_changed(void) {
    struct stat st;
    if (stat(g_auth_file, &st) != 0) return;
#ifdef __APPLE__
    struct timespec cur = st.st_mtimespec;
#else
    struct timespec cur = st.st_mtim;
#endif
    if (cur.tv_sec != g_htpasswd_mtime.tv_sec ||
        cur.tv_nsec != g_htpasswd_mtime.tv_nsec) {
        if (load_htpasswd(g_auth_file) < 0) {
            fprintf(stderr, "htpasswd: reload failed, keeping previous table\n");
        }
    }
}

int check_basic_auth(const char *header_value) {
    if (!g_auth_file[0]) return 1; /* auth disabled */
    htpasswd_reload_if_changed();
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

/* Same check as check_basic_auth, but for a caller that already has the
 * decoded username and password in hand (the /login form parser) and so has
 * no reason to re-encode them into a synthetic "Basic ..." header. */
int auth_check_credentials(const char *user, const char *pass) {
    if (!g_auth_file[0]) return 1; /* auth disabled */
    if (!user || !user[0] || !pass || !pass[0]) return 0;
    htpasswd_reload_if_changed();
    for (int i = 0; i < g_htpasswd_count; i++) {
        if (strcmp(user, g_htpasswd[i].user) == 0)
            return secret_matches(pass, g_htpasswd[i].secret);
    }
    return 0;
}

/* ---- Session cookie auth (AUTH_SESSION_FILE) ----
 *
 * Browser Basic-Auth credentials are cached per origin and cannot be saved by
 * any password manager (the dialog is native, not an HTML form), so a user
 * re-enters the password on every browser session. Session cookies fix that:
 * POST /login verifies against the htpasswd table once, then hands out an
 * opaque bearer token that the auth gate accepts.
 *
 * The token is never a password and no password is ever written: verification
 * happens against htpasswd at login time and the plaintext is discarded.
 * Deleting a user from htpasswd invalidates that user's sessions (see
 * auth_session_verify), which keeps the htpasswd file as the single authority.
 *
 * The file is space-separated and deliberately not JSON: auth.c has no JSON
 * dependency and the file is only ever touched from C. Users therefore cannot
 * contain spaces -- refused at create time with a warning rather than written
 * as a corrupt line. */
#define SESSION_COOKIE_DEFAULT "lume_session"
#define SESSION_TOKEN_HEX_LEN 32          /* 32 hex chars = 64 bits of entropy */
#define SESSION_TOKEN_BYTES (SESSION_TOKEN_HEX_LEN / 2)
#define SESSION_TTL_DEFAULT_DAYS 30
#define SESSION_MAX_LINES 512
#define SESSION_LINE_MAX 96               /* 32 token + ' ' + 64 user + ' ' + 11 expiry */

/* Two 48 KB frames would not fit below handle_client's own MAX_REQUEST_SIZE
 * buffer and the worker died inside strtok_r. File-scope is safe here: the
 * worker pool is prefork, so each worker process handles one connection at a
 * time, and the fast-path gate never re-enters these functions. */
static char g_session_read_buf[SESSION_MAX_LINES * SESSION_LINE_MAX];
static char g_session_keep_buf[SESSION_MAX_LINES * SESSION_LINE_MAX];

char g_session_cookie[64] = SESSION_COOKIE_DEFAULT;

static char g_session_file[MAX_PATH_SIZE] = "";
static long g_session_ttl_days = SESSION_TTL_DEFAULT_DAYS;
static int g_session_inited = 0;

/* Cookie names go straight into a Set-Cookie header, so only RFC 6265 token
 * characters are allowed. Anything else (a bare "=" would split the header, a
 * CRLF would inject a second one) is refused and the default name is kept. */
static int is_safe_cookie_name(const char *s) {
    if (!s || !s[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              strchr("-_.!~", (char)*p) != NULL)) {
            return 0;
        }
    }
    return 1;
}

static void session_lazy_init(void) {
    const char *e;
    if (g_session_inited) return;
    g_session_inited = 1;
    e = getenv("AUTH_SESSION_FILE");
    if (e && e[0]) {
        snprintf(g_session_file, sizeof(g_session_file), "%s", e);
        /* Validate now, not at first login: with an unusable path the server
         * would otherwise advertise a login form that can never log anyone in
         * (open() on a directory fails, so /login renders and POST always
         * answers 500). Dropping the path instead makes session auth degrade
         * to "off" -- /login becomes an ordinary gated 404 -- and the operator
         * sees why. */
        int fd = open(g_session_file, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            fprintf(stderr, "AUTH_SESSION_FILE: %s is not usable (%s); "
                            "session auth disabled\n", e, strerror(errno));
            g_session_file[0] = '\0';
        } else {
            close(fd);
        }
    }
    e = getenv("AUTH_SESSION_COOKIE");
    if (e && e[0] && strlen(e) + 1 <= sizeof(g_session_cookie) &&
        is_safe_cookie_name(e)) {
        snprintf(g_session_cookie, sizeof(g_session_cookie), "%s", e);
    } else if (e && e[0]) {
        fprintf(stderr, "AUTH_SESSION_COOKIE: value contains characters that "
                        "could alter the header; using %s instead\n",
                SESSION_COOKIE_DEFAULT);
    }
    e = getenv("AUTH_SESSION_TTL_DAYS");
    if (e && e[0]) {
        long d = atol(e);
        if (d > 0 && d <= 3650) g_session_ttl_days = d;
    }
}

/* Sessions are meaningless without a credential table to verify against, so
 * require both env vars. AUTH_SESSION_FILE alone would otherwise hand out
 * tokens for a server that cannot check anyone. */
int auth_session_enabled(void) {
    session_lazy_init();
    return g_session_file[0] != 0 && g_auth_file[0] != 0;
}

/* /login is the session-cookie counterpart of /logout: it must stay reachable
 * without any credentials, otherwise there is no way in. Exact-path rule as
 * is_logout_path -- a bare prefix match would let "/login-anything" through
 * the gate unauthenticated.
 *
 * It only says yes when sessions are on: without a session store the form
 * could be served outside the 401 gate and could never log anyone in, which
 * would be a dead page. With sessions off /login collapses to a normal gated
 * path and answers 401 like anything else. */
int is_login_path(const char *path) {
    if (!auth_session_enabled()) return 0;
    const char *p = path;
    while (*p && *p != '?') p++;
    return (int)(p - path) == 6 && strncmp(path, "/login", 6) == 0;
}

int auth_session_cookie_line(const char *token, char *out, size_t out_size) {
    session_lazy_init();
    if (!token || !token[0]) {
        /* Explicit deletion: Max-Age=0 makes the client drop it immediately. */
        snprintf(out, out_size, "%s=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0",
                 g_session_cookie);
        return 0;
    }
    long max_age = g_session_ttl_days * 86400;
    /* Secure is left off on purpose: behind an http:// reverse proxy the
     * cookie would be refused outright and login would silently never work.
     * SameSite=Lax already stops cross-site submission. */
    snprintf(out, out_size, "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%ld",
             g_session_cookie, token, max_age);
    return (int)max_age;
}

/* 16 bytes from /dev/urandom, hex-encoded so the token is always header-safe
 * and needs no quoting or escaping anywhere it travels. The fallback exists
 * because a token minted from zero entropy would be guessable, but it mixes
 * in pid and time so it is at least not constant across processes. */
/* Fail closed. A token minted from anything weaker than /dev/urandom would be
 * guessable, and a guessable session token is worse than a 500: the caller
 * can retry, but cannot be signed in as someone else. So this returns 0 on any
 * entropy failure instead of substituting a fallback generator. */
static int random_token_hex(char *out, size_t out_size) {
    unsigned char b[SESSION_TOKEN_BYTES];
    size_t off = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    while (off < sizeof(b)) {
        ssize_t n = read(fd, b + off, sizeof(b) - off);
        if (n <= 0) {
            close(fd);
            return 0;
        }
        off += (size_t)n;
    }
    close(fd);
    for (size_t i = 0; i < sizeof(b); i++) {
        if (!snprintf(out + i * 2, out_size - i * 2, "%02x", b[i])) return 0;
    }
    out[SESSION_TOKEN_HEX_LEN] = '\0';
    return 1;
}

/* Shared by verify/create/delete: hold the exclusive lock and read the whole
 * table into `buf` (nulsafe). Returns the fd to write through, or -1. */
static int session_lock_read(char *buf, size_t cap) {
    int fd = open(g_session_file, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) < 0) { close(fd); return -1; }
    char *p = buf;
    size_t left = cap - 1;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n <= 0) break;
        p += (size_t)n;
        left -= (size_t)n;
    }
    *p = '\0';
    if (lseek(fd, 0, SEEK_SET) < 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }
    return fd;
}

static void session_unlock_close(int fd) {
    flock(fd, LOCK_UN);
    close(fd);
}

static void session_unlock_write(int fd, const char *data) {
    ssize_t need = (ssize_t)strlen(data);
    ssize_t w = 0;
    while (w < need) {
        ssize_t n = write(fd, data + w, (size_t)(need - w));
        if (n <= 0) break;
        w += n;
    }
    if (ftruncate(fd, (off_t)w) < 0 || fsync(fd) < 0) {
        /* fsync failure is not fatal: the entry is lost on a crash, the user
         * just logs in again. Never error out the login for it. */
    }
    session_unlock_close(fd);
}

static int htpasswd_has_user(const char *user) {
    for (int i = 0; i < g_htpasswd_count; i++) {
        if (strcmp(user, g_htpasswd[i].user) == 0) return 1;
    }
    return 0;
}

/* Enumerate every "<name>=<value>" occurrence in a raw Cookie: header.
 * Returns 1 on a match and advances *cursor past the parsed segment so the
 * caller can keep looking; returns 0 when no further occurrence exists.
 *
 * Enumeration rather than a single lookup because duplicate names are legal on
 * the wire: cookies that differ only in Path or Domain attributes arrive as one
 * header line, and a logout that failed to delete the old copy leaves the stale
 * one behind. Names are compared case-sensitively (RFC 6265). */
static int cookie_next(const char *header, const char *name, const char **cursor,
                       char *out, size_t out_size) {
    if (!header || !header[0] || !cursor || !*cursor) return 0;
    const char *p = *cursor;
    size_t nlen = strlen(name);
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t seg = semi ? (size_t)(semi - p) : strlen(p);
        while (seg > 0 && (p[0] == ' ' || p[0] == '\t')) { p++; seg--; }
        const char *eq = NULL;
        for (size_t i = 0; i < seg; i++) {
            if (p[i] == '=') { eq = p + i; break; }
        }
        if (eq && (size_t)(eq - p) == nlen && strncmp(p, name, nlen) == 0) {
            const char *v = eq + 1;
            size_t vl = seg - (size_t)(v - p);
            while (vl > 0 && (v[0] == ' ' || v[0] == '\t')) { v++; vl--; }
            if (vl && v[0] == '"') { v++; vl--; }        /* optional DQUOTE */
            if (vl && v[vl - 1] == '"') vl--;
            if (vl && v[vl - 1] == ';') vl--;
            *cursor = semi ? semi + 1 : NULL;             /* resume after this one */
            if (vl > 0 && vl < out_size) {
                memcpy(out, v, vl);
                out[vl] = '\0';
                return 1;
            }
            return 0;
        }
        if (!semi) { *cursor = NULL; break; }
        p = semi + 1;
        *cursor = p;
    }
    return 0;
}

#define MAX_SESSION_COOKIES 8

/* Collect every session token carried by the Cookie header, in header order,
 * into toks[]; returns how many were stored (capped at max_toks).
 *
 * Duplicate session cookies are legal on the wire -- they differ only in Path or
 * Domain attributes, or a logout failed to delete the old copy -- and both the
 * gate and the logout path have to see all of them. Reading only the first meant
 * a leftover copy shadowed the live session (caller served as the wrong user,
 * or a bare 401 when the stale copy was the invalid one) and left /logout unable
 * to revoke anything but one of the two tokens. */
static int session_cookie_tokens(const char *cookie_header,
                                 char toks[][SESSION_TOKEN_HEX_LEN + 1], int max_toks) {
    int n = 0;
    const char *cursor = cookie_header;
    char tok[SESSION_TOKEN_HEX_LEN + 1];
    while (n < max_toks &&
           cookie_next(cookie_header, g_session_cookie, &cursor, tok, sizeof(tok))) {
        if (strlen(tok) == SESSION_TOKEN_HEX_LEN) {
            memcpy(toks[n], tok, SESSION_TOKEN_HEX_LEN + 1);
            n++;
        }
        if (!cursor) break;
    }
    return n;
}

int auth_session_verify(const char *cookie_header, char *user_out, size_t out_size) {
    if (!auth_session_enabled() || out_size == 0) return 0;
    /* All candidates are collected up front and the first one that actually
     * resolves wins, so a stale duplicate cannot shadow the live session. */
    char toks[MAX_SESSION_COOKIES][SESSION_TOKEN_HEX_LEN + 1];
    int ntok = session_cookie_tokens(cookie_header, toks, MAX_SESSION_COOKIES);
    if (ntok == 0) return 0;
    char *buf = g_session_read_buf;
    int fd = session_lock_read(buf, sizeof(g_session_read_buf));
    if (fd < 0) return 0;
    htpasswd_reload_if_changed();
    long now = (long)time(NULL);
    char *keep = g_session_keep_buf;
    keep[0] = '\0';
    int found = 0;
    /* verify() runs on every request, so the common case is "session still
     * valid, nothing expired". That case must not rewrite the file: writing +
     * ftruncate + fsync on every hit would turn a busy authed server into a
     * synchronous disk-write loop. Only a real prune is worth persisting. */
    int dropped = 0;
    char *save;
    char *line = strtok_r(buf, "\n", &save);
    while (line) {
        char tok[SESSION_TOKEN_HEX_LEN + 1], user[128], exp[32];
        if (sscanf(line, "%32s %127s %31s", tok, user, exp) == 3) {
            long e = atol(exp);
            if (e > now && htpasswd_has_user(user)) {
                snprintf(keep + strlen(keep),
                         sizeof(g_session_keep_buf) - strlen(keep), "%s %s %s\n", tok, user, exp);
                /* The newest matching session wins, not the first cookie in the
                 * header: the file is append-ordered, so a later line is a
                 * later login. Cookie order is not a usable signal -- browsers
                 * send duplicates in an unspecified order, and taking the first
                 * one is exactly what let a stale copy shadow the live session
                 * (observed on the live box: test's leftover cookie kept the
                 * dashboard on role=client right after logging in as admin). */
                for (int i = 0; i < ntok; i++) {
                    if (strcmp(tok, toks[i]) == 0) {
                        snprintf(user_out, out_size, "%s", user);
                        found = 1;
                    }
                }
            } else {
                dropped = 1;
            }
        } else {
            dropped = 1;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    if (dropped)
        session_unlock_write(fd, keep);
    else
        session_unlock_close(fd);
    return found;
}

int auth_session_valid(const char *cookie_header) {
    /* The gate only needs a yes/no. The worker pool is prefork (separate
     * processes, no threads), so a function-static buffer is race-free here. */
    static char user[128];
    return auth_session_verify(cookie_header, user, sizeof(user));
}

int auth_session_create(const char *user, char *token_out, size_t out_size) {
    if (!auth_session_enabled() || !user || !user[0]) return 0;
    if (strchr(user, ' ') || strchr(user, '\t') || strchr(user, '\n')) {
        fprintf(stderr, "session: refusing a username containing whitespace\n");
        return 0;
    }
    if (strlen(user) + 1 > 64) {
        fprintf(stderr, "session: refusing user with a name over 64 chars\n");
        return 0;
    }
    htpasswd_reload_if_changed();
    if (!htpasswd_has_user(user)) return 0;
    if (!random_token_hex(token_out, out_size)) {
        /* Never fall back to a weak generator: a guessable token is worse
         * than a 500 the user can retry. */
        fprintf(stderr, "session: entropy source unavailable, refusing to mint a token\n");
        return 0;
    }
    char *buf = g_session_read_buf;
    int fd = session_lock_read(buf, sizeof(g_session_read_buf));
    if (fd < 0) return 0;
    long now = (long)time(NULL);
    char *keep = g_session_keep_buf;
    keep[0] = '\0';
    char *save;
    char *line = strtok_r(buf, "\n", &save);
    while (line) {
        char tok[SESSION_TOKEN_HEX_LEN + 1], u[128], exp[32];
        if (sscanf(line, "%32s %127s %31s", tok, u, exp) == 3 && atol(exp) > now) {
            snprintf(keep + strlen(keep), sizeof(g_session_keep_buf) - strlen(keep),
                     "%s %s %s\n", tok, u, exp);
        }
        line = strtok_r(NULL, "\n", &save);
    }
    snprintf(keep + strlen(keep), sizeof(g_session_keep_buf) - strlen(keep), "%s %s %ld\n",
             token_out, user, now + g_session_ttl_days * 86400L);
    session_unlock_write(fd, keep);
    return 1;
}

int auth_session_delete(const char *cookie_header) {
    if (!auth_session_enabled()) return 0;
    /* Revoke every token the caller presented, not just the first: with a
     * duplicate cookie left behind, deleting only one copy meant /logout left the
     * other session live, so the stale identity came straight back on the next
     * request. */
    char toks[MAX_SESSION_COOKIES][SESSION_TOKEN_HEX_LEN + 1];
    int ntok = session_cookie_tokens(cookie_header, toks, MAX_SESSION_COOKIES);
    if (ntok == 0) return 0;
    char *buf = g_session_read_buf;
    int fd = session_lock_read(buf, sizeof(g_session_read_buf));
    if (fd < 0) return 0;
    long now = (long)time(NULL);
    int removed = 0;
    char *keep = g_session_keep_buf;
    keep[0] = '\0';
    char *save;
    char *line = strtok_r(buf, "\n", &save);
    while (line) {
        char tok[SESSION_TOKEN_HEX_LEN + 1], u[128], exp[32];
        if (sscanf(line, "%32s %127s %31s", tok, u, exp) == 3 && atol(exp) > now) {
            int drop = 0;
            for (int i = 0; i < ntok; i++) {
                if (strcmp(tok, toks[i]) == 0) { drop = 1; break; }
            }
            if (drop) {
                removed = 1;
            } else {
                snprintf(keep + strlen(keep), sizeof(g_session_keep_buf) - strlen(keep),
                         "%s %s %s\n", tok, u, exp);
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    session_unlock_write(fd, keep);
    return removed;
}

void auth_401_as_json(HttpResponse *response, const char *path) {
    if (!auth_session_enabled() || !path || strncmp(path, "/api/", 5) != 0) return;
    /* strdup before free: on failure keep the HTML page rather than tearing
     * the response apart. set_error_response always malloc's its body (either
     * the error page or the inline template), so free() is safe here. */
    char *old = response->body;
    char *json = strdup("{\"error\":\"unauthorized\"}");
    if (!json) return;
    free(old);
    response->body = json;
    response->body_length = (int)strlen(json);
    snprintf(response->content_type, sizeof(response->content_type),
             "application/json");
}
