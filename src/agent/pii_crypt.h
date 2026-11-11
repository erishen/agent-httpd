/* Framework-layer PII field encryption (option (c) of lume-core
 * docs/PII_AT_REST.md §5, selected 2026-11-10).
 *
 * Scope: field-granular AEAD at the two uniform DB entry points
 * (`db_query_json()` / `db_write_exec()`), so the DSL never sees a key and
 * the SQLite path gets the same protection as MySQL/PostgreSQL. It is a
 * framework responsibility, not a language builtin: no interp/typecheck/
 * codegen registration, no core parity surface, one tree to maintain.
 *
 * Wire format: `lume1.<key-id>.<base64url(nonce ‖ ciphertext ‖ tag)>`
 *   - ChaCha20-Poly1305 (RFC 8439), 12-byte CSPRNG nonce per encryption
 *   - AAD = "<table>:<column>", so a ciphertext moved to another column or
 *     another table fails authentication instead of decrypting
 *   - key-id lets a rotated key coexist with retired rows (the hole
 *     crypto_pii.py has, where rotating SECRET_KEY orphans every row)
 *
 * Compatibility: anything not starting with `lume1.` is returned unchanged
 * (legacy plaintext rows stay readable, never a 500, never an empty string).
 * Missing/short key is fail-closed: encrypt errors instead of storing
 * plaintext.
 *
 * Configuration (environment, read in C — never exposed to the DSL):
 *   LUME_PII_COLUMNS   comma-separated "table.column" list; empty = off
 *   LUME_PII_SECRET    32-byte key as 64 hex chars
 *   LUME_PII_KEY_ID    current key id, default "1"
 *   LUME_PII_SECRET_PREV / LUME_PII_KEY_ID_PREV  optional retired key
 */
#ifndef AGENTHTTPD_PII_CRYPT_H
#define AGENTHTTPD_PII_CRYPT_H

#include <stddef.h>

#define PII_TOKEN_PREFIX "lume1."
#define PII_MAX_NAME 64
#define PII_MAX_COLS 64  /* max INSERT column list / policy rules per statement */

/* ---- AEAD primitives (RFC 8439), exposed for the pinned test vectors --- */

int pii_aead_seal(const unsigned char key[32], const unsigned char nonce[12],
                  const unsigned char *aad, size_t aadlen,
                  const unsigned char *pt, size_t ptlen,
                  unsigned char *ct, unsigned char tag[16]);

int pii_aead_open(const unsigned char key[32], const unsigned char nonce[12],
                  const unsigned char *aad, size_t aadlen,
                  const unsigned char *ct, size_t ctlen,
                  const unsigned char tag[16], unsigned char *pt);

/* ---- policy ---------------------------------------------------------- */

void pii_policy_reset(const char *columns_csv, const char *secret_hex,
                      const char *key_id);
void pii_policy_set_previous(const char *key_id, const char *secret_hex);
/* Load the policy from the environment (LUME_PII_*). Safe to call once. */
void pii_policy_from_env(void);
int pii_configured(void);
int pii_is_column(const char *table, const char *column);

/* ---- value transforms (malloc'd results, caller frees) ---------------- */

/* Returns a token, or NULL with `err` set on failure; NULL with an empty
 * `err` means "not a PII column, write the value unchanged". */
char *pii_encrypt_value(const char *table, const char *column, const char *plain,
                        char *err, size_t errsz);

/* Returns the plaintext (or the input unchanged for legacy/plain values),
 * or NULL with `err` set when a `lume1.` token fails authentication. */
char *pii_decrypt_value(const char *table, const char *column, const char *token,
                        char *err, size_t errsz);

/* ---- bind-level hook (all backends) ----------------------------------- */

/* Encrypts the PII-declared positions of an INSERT before the driver binds
 * them. Backend-independent: it only rewrites the parameter array, so the
 * SQLite / MySQL / PostgreSQL writers can share one implementation.
 * On success `*bind_ptr` points at the array to bind (== `params` when
 * nothing changed) and `owned`/`*n_owned` hold the tokens the caller must
 * free. Returns 1 with `err` set on a fail-closed refusal. */
int pii_bind_encode(const char *sql, const char **params, int nparams,
                    const char ***bind_ptr, char **owned, int *n_owned,
                    char *err, size_t errsz);

/* ---- write-path helper ----------------------------------------------- */

/* For `INSERT INTO t (a, b, c) VALUES (?, ?, ?)`: fills `idx` with the
 * 0-based parameter positions of PII columns and `*n` with their count.
 * Returns 0 on a supported shape, non-zero on anything it refuses to guess
 * at (no column list, non-VALUES forms, named parameters, UPDATE/UPSERT).
 * When `table_out` is non-NULL it receives the target table name. */
int pii_insert_param_columns(const char *sql, int *idx, int max, int *n);

/* Same parse, but carrying the column name so the write hook knows which
 * declared column each parameter belongs to. */
typedef struct {
    int param;                    /* 0-based parameter position */
    char column[PII_MAX_NAME];
} pii_target;

int pii_insert_targets(const char *sql, pii_target *out, int max, int *n);

/* Does this table have any policy-declared column? Used to fail closed on
 * INSERT shapes the parser refuses to attribute. */
int pii_table_has_pii(const char *table);
int pii_insert_table(const char *sql, char *table_out, size_t table_sz);

/* Table name of a single-table SELECT (empty string when the statement is
 * too complex to attribute safely — the read hook then leaves values
 * untouched rather than mis-keying the AAD). */
void pii_select_table(const char *sql, char *table_out, size_t table_sz);

#endif /* AGENTHTTPD_PII_CRYPT_H */
