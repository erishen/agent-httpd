/* ---- Basic Auth (RFC 7617) ----
 * htpasswd file: lines of "user:secret". secret is either plaintext or a
 * crypt(3) hash (DES/MD5/SHA-* auto-detected: a hash is anything starting
 * with "$<id>$" or a 13-char DES string). Plaintext secrets must contain
 * ':' or start with '{' to stay unambiguous (e.g. "{SHA}..." or
 * "{PLAIN}"). Mismatching lines are skipped - a malformed file can never
 * become "allow all". Empty g_auth_file disables auth entirely. */

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

char g_auth_file[MAX_PATH_SIZE] = "";
char g_auth_realm[128] = DEFAULT_AUTH_REALM;

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
    int acc = 0, bits = 0;
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

static int secret_matches(const char *supplied, const char *stored) {
    int stored_is_hash = (stored[0] == '$') ||
                         (strlen(stored) == 13 && strspn(stored, "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") == 13);
    if (!stored_is_hash) {
        return ct_eq(supplied, stored); /* plaintext (incl. {SHA}-style) */
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
    for (int i = 0; i < g_htpasswd_count; i++) {
        if (strcmp(creds, g_htpasswd[i].user) == 0) {
            return secret_matches(pass, g_htpasswd[i].secret);
        }
    }
    return 0;
}
