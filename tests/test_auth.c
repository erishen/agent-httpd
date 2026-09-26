/* Unit tests for src/security/auth.c: base64 credential decoding, Basic
 * Auth checks and the hash-only htpasswd load policy (privacy audit L2).
 * Compiled standalone with the module under ASan/UBSan (see Makefile
 * test-unit); no live server needed. crypt(3)-dependent assertions degrade
 * gracefully on platforms whose crypt lacks the scheme (macOS: DES only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <crypt.h>
#endif

#include "internal.h" /* g_auth_file, b64_decode, check_basic_auth, load_htpasswd */

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            printf("PASS: %s\n", #cond);                                \
        } else {                                                        \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__);            \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* Reference base64 encoder so credentials in tests never depend on a
 * hand-typed constant being right. */
static void b64_encode(const char *in, char *out) {
    static const char A[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t l = strlen(in), o = 0;
    for (size_t i = 0; i < l; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        int rem = (int)(l - i);
        if (rem > 1) v |= (unsigned char)in[i + 1] << 8;
        if (rem > 2) v |= (unsigned char)in[i + 2];
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = rem > 1 ? A[(v >> 6) & 63] : '=';
        out[o++] = rem > 2 ? A[v & 63] : '=';
    }
    out[o] = '\0';
}

/* "Basic <base64(user:pass)>" header from plaintext credentials. */
static void make_basic(const char *user, const char *pass, char *out,
                       size_t out_size) {
    char creds[128], b64[192];
    snprintf(creds, sizeof creds, "%s:%s", user, pass);
    b64_encode(creds, b64);
    snprintf(out, out_size, "Basic %s", b64);
}

static void write_htpasswd(const char *name, const char *content) {
    char path[256];
    snprintf(path, sizeof path, "/tmp/ah_test_%s", name);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("fopen htpasswd");
        exit(1);
    }
    fputs(content, f);
    fclose(f);
}

static void set_auth_file(const char *name) {
    snprintf(g_auth_file, sizeof g_auth_file, "/tmp/ah_test_%s", name);
}

int main(void) {
    char hdr[256], out[256];

    /* ---- b64_decode ---- */
    b64_encode("alice:password123", out);
    CHECK(strcmp(out, "YWxpY2U6cGFzc3dvcmQxMjM=") == 0);

    b64_decode("YWxpY2U6cGFzc3dvcmQxMjM=", out, sizeof out);
    CHECK(strcmp(out, "alice:password123") == 0);

    /* '=' padding terminates; short groups round down. */
    b64_decode("QUI=", out, sizeof out); /* "AB" */
    CHECK(strcmp(out, "AB") == 0);
    b64_decode("QQ==", out, sizeof out); /* "A" */
    CHECK(strcmp(out, "A") == 0);

    /* Invalid alphabet bytes stop decoding (sentinel table), never wrap. */
    b64_decode("YWxpY2U6!@#", out, sizeof out);
    CHECK(strcmp(out, "alice:") == 0);

    /* Truncation: out_size includes the NUL, output is cut cleanly. */
    b64_decode("YWxpY2U6cGFzc3dvcmQxMjM=", out, 6);
    CHECK(strcmp(out, "alice") == 0);

    /* ---- auth disabled: empty g_auth_file allows everything ---- */
    g_auth_file[0] = '\0';
    CHECK(check_basic_auth(NULL) == 1);
    CHECK(check_basic_auth("garbage") == 1);

    /* ---- hash-only load policy: plaintext rejected by default ---- */
    unsetenv("AGENTHTTPD_ALLOW_WEAK_AUTH");
    write_htpasswd("plain", "alice:password123\n");
    set_auth_file("plain");
    CHECK(load_htpasswd(g_auth_file) == -1);
    /* All entries skipped -> the file can never become "allow all". */
    make_basic("alice", "password123", hdr, sizeof hdr);
    CHECK(check_basic_auth(hdr) == 0);

    /* Mixed file: strong entry loads, weak sibling is skipped forever. */
    const char *probe = crypt("probe", "$6$abcdefgh");
    if (probe && strncmp(probe, "$6$", 3) == 0) {
        char mixed[512];
        snprintf(mixed, sizeof mixed,
                 "alice:password123\ncarol:%s\n", probe);
        write_htpasswd("mixed", mixed);
        set_auth_file("mixed");
        CHECK(load_htpasswd(g_auth_file) == 0);
        make_basic("alice", "password123", hdr, sizeof hdr);
        CHECK(check_basic_auth(hdr) == 0); /* weak entry skipped */
        make_basic("carol", "carolpass", hdr, sizeof hdr);
        CHECK(check_basic_auth(hdr) == 1); /* strong entry matches */
        make_basic("carol", "wrongpass", hdr, sizeof hdr);
        CHECK(check_basic_auth(hdr) == 0);
    } else {
        printf("SKIP: $6$ crypt unavailable on this platform\n");
    }

    /* ---- dev escape hatch: weak secrets accepted after loud warning ---- */
    setenv("AGENTHTTPD_ALLOW_WEAK_AUTH", "1", 1);
    CHECK(load_htpasswd(g_auth_file) == 0); /* same file now loads */
    make_basic("alice", "password123", hdr, sizeof hdr);
    CHECK(check_basic_auth(hdr) == 1);
    make_basic("alice", "wrongpass", hdr, sizeof hdr);
    CHECK(check_basic_auth(hdr) == 0);
    unsetenv("AGENTHTTPD_ALLOW_WEAK_AUTH");

    /* DES crypt path (works on macOS and glibc alike, weak scheme -> needs
     * the escape hatch). */
    const char *des = crypt("password123", "sa");
    if (des && strlen(des) == 13) {
        char f[512];
        snprintf(f, sizeof f, "carol:%s\n", des);
        write_htpasswd("des", f);
        setenv("AGENTHTTPD_ALLOW_WEAK_AUTH", "1", 1);
        set_auth_file("des");
        CHECK(load_htpasswd(g_auth_file) == 0);
        make_basic("carol", "password123", hdr, sizeof hdr);
        CHECK(check_basic_auth(hdr) == 1);
        make_basic("carol", "wrongpass", hdr, sizeof hdr);
        CHECK(check_basic_auth(hdr) == 0);
        unsetenv("AGENTHTTPD_ALLOW_WEAK_AUTH");
    } else {
        printf("SKIP: DES crypt unavailable on this platform\n");
    }

    /* ---- credential parsing edge cases ---- */
    setenv("AGENTHTTPD_ALLOW_WEAK_AUTH", "1", 1);
    set_auth_file("plain"); /* alice:password123 */
    CHECK(load_htpasswd(g_auth_file) == 0);
    /* Unknown user never matches. */
    make_basic("bob", "password123", hdr, sizeof hdr);
    CHECK(check_basic_auth(hdr) == 0);
    /* Wrong scheme / missing header. */
    CHECK(check_basic_auth("Bearer YWxpY2U6") == 0);
    CHECK(check_basic_auth(NULL) == 0);
    CHECK(check_basic_auth("Basic") == 0);
    CHECK(check_basic_auth("Basic ") == 0);
    /* Decoded credentials without a ':' separator are rejected. */
    b64_encode("justuser", out);
    snprintf(hdr, sizeof hdr, "Basic %s", out);
    CHECK(check_basic_auth(hdr) == 0);
    unsetenv("AGENTHTTPD_ALLOW_WEAK_AUTH");

    /* ---- bcrypt ($2y$, htpasswd -B): portable verify path ---- */
    /* htpasswd -B on macOS emits $2y$; libc crypt(3) can never verify it
     * (DES-only on macOS, no bcrypt in glibc), so auth.c must fall back to
     * the bundled bcrypt_verify(). Real vector: password admin123. */
    write_htpasswd("bcrypt",
                   "admin:$2y$05$Y5kyTRNmGmleUf6Htnklq..AbXM/5ujHUpQDpx9s8l6BTra273EHK\n");
    set_auth_file("bcrypt");
    CHECK(load_htpasswd(g_auth_file) == 0);
    make_basic("admin", "admin123", hdr, sizeof hdr);
    CHECK(check_basic_auth(hdr) == 1);
    make_basic("admin", "admin124", hdr, sizeof hdr);
    CHECK(check_basic_auth(hdr) == 0);
    /* Wrong scheme on the same entry still never matches. */
    CHECK(check_basic_auth("Bearer YWxpY2U6") == 0);

    snprintf(g_auth_file, sizeof g_auth_file, "/tmp/ah_test_plain");
    unlink(g_auth_file);
    set_auth_file("mixed"); unlink(g_auth_file);
    set_auth_file("des");   unlink(g_auth_file);
    set_auth_file("bcrypt"); unlink(g_auth_file);

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
