/* Unit tests for the framework-layer PII field cipher (src/agent/pii_crypt.c).
 *
 * The AEAD is pinned to RFC 8439 §2.8.2: the expected ciphertext and tag
 * below were produced independently by OpenSSL (via node's
 * `chacha20-poly1305`) from the RFC inputs and match the RFC text byte for
 * byte, so a bug in this tree cannot silently redefine "correct".
 *
 * Compiled standalone (see Makefile test-unit); no server, no database.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pii_crypt.h"

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

#define HEX_KEY_RFC                                                     \
    "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
#define HEX_NONCE_RFC "070000004041424344454647"
#define HEX_AAD_RFC   "50515253c0c1c2c3c4c5c6c7"

static const char *RFC_PT =
    "Ladies and Gentlemen of the class of '99: If I could offer you only "
    "one tip for the future, sunscreen would be it.";

static const char *RFC_CT =
    "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
    "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
    "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
    "3ff4def08e4b7a9de576d26586cec64b6116";

static const char *RFC_TAG = "1ae10b594f09e26a7e902ecbd0600691";

/* 32 bytes of hex key for the policy tests. */
#define POLICY_KEY                                                     \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

static void from_hex(const char *hex, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

/* ---- 1. RFC 8439 §2.8.2 vector --------------------------------------- */

static void test_rfc8439_vector(void) {
    unsigned char key[32], nonce[12], aad[12];
    unsigned char ct[114], tag[16], pt[114];
    from_hex(HEX_KEY_RFC, key, 32);
    from_hex(HEX_NONCE_RFC, nonce, 12);
    from_hex(HEX_AAD_RFC, aad, 12);

    int rc = pii_aead_seal(key, nonce, aad, 12, (const unsigned char *)RFC_PT,
                           strlen(RFC_PT), ct, tag);
    CHECK(rc == 0);

    char hex[256];
    for (size_t i = 0; i < strlen(RFC_PT); i++) snprintf(hex + 2 * i, 3, "%02x", ct[i]);
    CHECK(strcmp(hex, RFC_CT) == 0);

    char taghex[40];
    for (int i = 0; i < 16; i++) snprintf(taghex + 2 * i, 3, "%02x", tag[i]);
    CHECK(strcmp(taghex, RFC_TAG) == 0);

    rc = pii_aead_open(key, nonce, aad, 12, ct, strlen(RFC_PT), tag, pt);
    CHECK(rc == 0);
    CHECK(memcmp(pt, RFC_PT, strlen(RFC_PT)) == 0);
}

/* ---- 2. tamper rejection -------------------------------------------- */

static void test_tamper(void) {
    unsigned char key[32], nonce[12], aad[12], ct[32], tag[16], pt[32];
    from_hex(POLICY_KEY, key, 32);
    from_hex(HEX_NONCE_RFC, nonce, 12);
    from_hex(HEX_AAD_RFC, aad, 12);
    CHECK(pii_aead_seal(key, nonce, aad, 12, (const unsigned char *)"13800005678",
                        11, ct, tag) == 0);
    CHECK(pii_aead_open(key, nonce, aad, 12, ct, 11, tag, pt) == 0);

    ct[3] ^= 0x01; /* flip one ciphertext bit */
    CHECK(pii_aead_open(key, nonce, aad, 12, ct, 11, tag, pt) != 0);
    ct[3] ^= 0x01;

    tag[15] ^= 0x80; /* flip one tag bit */
    CHECK(pii_aead_open(key, nonce, aad, 12, ct, 11, tag, pt) != 0);
    tag[15] ^= 0x80;

    /* AAD relocation must be rejected: same ciphertext, other column. */
    unsigned char aad2[12];
    from_hex("50515253c0c1c2c3c4c5c6c8", aad2, 12);
    CHECK(pii_aead_open(key, nonce, aad2, 12, ct, 11, tag, pt) != 0);

    /* Wrong key must be rejected. */
    unsigned char wrong[32];
    from_hex("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
             wrong, 32);
    CHECK(pii_aead_open(wrong, nonce, aad, 12, ct, 11, tag, pt) != 0);
}

/* ---- 3. policy surface: round-trip, nonce freshness, passthrough ------ */

static void test_policy(void) {
    char err[160] = "";
    pii_policy_reset("bookings.name,bookings.phone", POLICY_KEY, "1");
    CHECK(pii_configured() == 1);
    CHECK(pii_is_column("bookings", "phone") == 1);
    CHECK(pii_is_column("bookings", "note") == 0);
    /* Table-qualified policy must not leak onto a same-named column of
     * another table. */
    CHECK(pii_is_column("items", "name") == 0);

    char *t1 = pii_encrypt_value("bookings", "phone", "13800005678", err, sizeof err);
    CHECK(t1 != NULL);
    CHECK(strncmp(t1, "lume1.1.", 8) == 0);

    char *p1 = pii_decrypt_value("bookings", "phone", t1, err, sizeof err);
    CHECK(p1 != NULL);
    CHECK(strcmp(p1, "13800005678") == 0);

    /* Same plaintext twice must not produce the same token (fresh nonce). */
    char *t2 = pii_encrypt_value("bookings", "phone", "13800005678", err, sizeof err);
    CHECK(t2 != NULL && strcmp(t1, t2) != 0);

    /* Relocating the token to another column must fail, not silently
     * decrypt. */
    char *bad = pii_decrypt_value("bookings", "name", t1, err, sizeof err);
    CHECK(bad == NULL);
    CHECK(err[0] != '\0');

    /* Legacy plaintext (no lume1. prefix) passes through unchanged: the
     * fastapi-web contract, so pre-existing rows stay readable. */
    char *legacy = pii_decrypt_value("bookings", "phone", "13800005678", err, sizeof err);
    CHECK(legacy != NULL && strcmp(legacy, "13800005678") == 0);

    /* Non-PII column: encrypt is a no-op passthrough (NULL, no error). */
    CHECK(pii_encrypt_value("bookings", "note", "hello", err, sizeof err) == NULL);
    CHECK(err[0] == '\0');

    free(t1); free(t2); free(p1); free(legacy);
}

/* ---- 4. fail-closed when the key is missing -------------------------- */

static void test_missing_key(void) {
    char err[160] = "";
    pii_policy_reset("bookings.phone", "", "1");
    CHECK(pii_configured() == 1);
    char *t = pii_encrypt_value("bookings", "phone", "13800005678", err, sizeof err);
    CHECK(t == NULL);
    CHECK(err[0] != '\0'); /* must error, never store plaintext */

    /* Short / non-hex key is also rejected up front. */
    pii_policy_reset("bookings.phone", "abcd", "1");
    char *t2 = pii_encrypt_value("bookings", "phone", "13800005678", err, sizeof err);
    CHECK(t2 == NULL);
    CHECK(err[0] != '\0');
}

/* ---- 5. key rotation: token under key-id 1 still opens with prev ------ */

static void test_rotation(void) {
    char err[160] = "";
    pii_policy_reset("bookings.phone", POLICY_KEY, "2");
    char *tok = pii_encrypt_value("bookings", "phone", "13800005678", err, sizeof err);
    CHECK(tok != NULL && strncmp(tok, "lume1.2.", 8) == 0);

    /* Only key-id 2 is configured: the token opens. */
    char *p = pii_decrypt_value("bookings", "phone", tok, err, sizeof err);
    CHECK(p != NULL && strcmp(p, "13800005678") == 0);

    /* Switch current key to id 3 and keep 2 as retired; the old row must
     * stay readable (the crypto_pii.py hole this design closes). */
    pii_policy_reset("bookings.phone",
                     "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                     "3");
    pii_policy_set_previous("2",
                            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    char *p2 = pii_decrypt_value("bookings", "phone", tok, err, sizeof err);
    CHECK(p2 != NULL && strcmp(p2, "13800005678") == 0);

    free(tok); free(p); free(p2);
}

/* ---- 6. empty plaintext --------------------------------------------- */

static void test_empty(void) {
    char err[160] = "";
    pii_policy_reset("bookings.name", POLICY_KEY, "1");
    char *t = pii_encrypt_value("bookings", "name", "", err, sizeof err);
    CHECK(t != NULL);
    char *p = pii_decrypt_value("bookings", "name", t, err, sizeof err);
    CHECK(p != NULL && strcmp(p, "") == 0);
    free(t); free(p);
}

/* ---- 7. INSERT column mapping --------------------------------------- */

static void test_insert_mapping(void) {
    /* The write hook needs "which ? is which column" from the statement
     * text; a wrong index map would encrypt the wrong field. */
    pii_policy_reset("bookings.name,bookings.phone", POLICY_KEY, "1");

    int idx[8], n = 0;
    CHECK(pii_insert_param_columns(
              "INSERT INTO bookings (item_id, item_name, name, phone, note) "
              "VALUES (?, ?, ?, ?, ?)",
              idx, 8, &n) == 0);
    CHECK(n == 2);
    CHECK(idx[0] == 2 && idx[1] == 3); /* name and phone, 0-based */

    int idx2[8], n2 = 0;
    CHECK(pii_insert_param_columns("INSERT INTO bookings (name, phone, note) "
                                   "VALUES (?, ?, ?)",
                                   idx2, 8, &n2) == 0);
    CHECK(n2 == 2);
    CHECK(idx2[0] == 0 && idx2[1] == 1);

    /* Mixed placeholders and literals — the shape this service actually
     * writes (`status` is a literal 0) — must still map the right parameter
     * indices. Regression: the first parser refused every such statement and
     * the hook then fell through to a plaintext write. */
    int idx4[8], n4 = 0;
    CHECK(pii_insert_param_columns(
              "INSERT INTO bookings (status, name, phone, created_at) "
              "VALUES (0, ?, ?, ?)",
              idx4, 8, &n4) == 0);
    CHECK(n4 == 2);
    CHECK(idx4[0] == 0 && idx4[1] == 1);

    /* A literal in a PII column bypasses encryption, so it must be refused
     * (distinct code -2, not a silent success). */
    int idx5[8], n5 = 0;
    CHECK(pii_insert_param_columns(
              "INSERT INTO bookings (name, phone) VALUES (?, '13900000000')",
              idx5, 8, &n5) == -2);

    /* Unsupported shapes must be refused rather than guessed at. */
    int idx3[8], n3 = 0;
    CHECK(pii_insert_param_columns("INSERT INTO bookings VALUES (?, ?)",
                                   idx3, 8, &n3) != 0);
    CHECK(pii_insert_param_columns("UPDATE bookings SET phone = ? WHERE id = ?",
                                   idx3, 8, &n3) != 0);
}

/* Runs in its own process: the point is that a read-only process, which
 * only ever asks pii_is_column(), must still load the policy from the
 * environment. Regression: a fresh server that served GET before any write
 * returned the raw lume1.… tokens instead of decrypting them. */
static void test_env_lazy_load(void) {
    setenv("LUME_PII_COLUMNS", "bookings.phone", 1);
    setenv("LUME_PII_SECRET",
           "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 1);
    setenv("LUME_PII_KEY_ID", "7", 1);

    CHECK(pii_is_column("bookings", "phone") == 1);
    CHECK(pii_is_column("bookings", "name") == 0);
    CHECK(pii_configured() == 1);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "env") == 0) {
        test_env_lazy_load();
        printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK-env",
               failures, failures == 1 ? "" : "s");
        return failures ? 1 : 0;
    }
    test_rfc8439_vector();
    test_tamper();
    test_policy();
    test_missing_key();
    test_rotation();
    test_empty();
    test_insert_mapping();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
