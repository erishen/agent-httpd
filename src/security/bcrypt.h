/* bcrypt.h — portable bcrypt password verification for agent-httpd.
 *
 * macOS/BSD libc crypt() only implements the legacy DES scheme, so a
 * bcrypt-form htpasswd ($2a$/$2b$/$2y$) can never verify there. This module
 * implements the verify direction of the OpenBSD bcrypt algorithm
 * (Blowfish-based, public domain) so authentication behaves identically on
 * every platform. Used by auth.c when the stored secret is a bcrypt hash.
 */
#ifndef AGENT_HTTPD_BCRYPT_H
#define AGENT_HTTPD_BCRYPT_H

/* Verify `pass` against a stored bcrypt hash of the form
 * "$2[abxy]$<cost>$<22-char-salt><31-char-hash>".
 * Returns 1 on match, 0 on mismatch or malformed input. Constant-time
 * comparison against the stored hash. */
int bcrypt_verify(const char *pass, const char *stored);

/* Returns 1 when `stored` starts with a supported bcrypt prefix. */
int bcrypt_is_hash(const char *stored);

#endif /* AGENT_HTTPD_BCRYPT_H */
