#ifndef MINIJSON_H
#define MINIJSON_H

#include <stddef.h>

/* Dependency-free JSON helpers shared by the chat modules (llm/agent/tools).
 * Two halves:
 *   - sbuf: growable, always-NUL-terminated string builder (request bodies,
 *     accumulated SSE lines, tool results).
 *   - tolerant reader: search-style access into JSON text WITHOUT a DOM —
 *     jfind_value locates a top-level key, jread_string unescapes a string
 *     in place. Good enough for OpenAI-compatible SSE lines and MCP
 *     payloads; deliberately not a validating parser. */

/* ---- growable string buffer ------------------------------------- */

typedef struct {
    char *p;      /* NULL until first append; always NUL-terminated */
    size_t len;
    size_t cap;
    int oom;      /* sticky: further appends become no-ops */
} sbuf;

void sb_mem(sbuf *b, const char *s, size_t n);
void sb_str(sbuf *b, const char *s);
void sb_chr(sbuf *b, char c);
/* Append s as a quoted JSON string (control chars escaped, non-ASCII
 * passes through verbatim). */
void sb_json_str(sbuf *b, const char *s);

/* ---- tolerant reader --------------------------------------------- */

/* Skip whitespace, returning the advanced pointer. */
const char *jws(const char *p);
/* Read the JSON string at *pp (positioned on the opening quote), unescaping
 * (\uXXXX -> UTF-8 with surrogate pairs; lone surrogates -> U+FFFD) into
 * out (truncating, always NUL-terminated). Non-escape bytes pass through
 * verbatim so UTF-8 text survives byte-for-byte. Returns the position
 * after the closing quote, or NULL on malformed input. */
const char *jread_string(const char **pp, char *out, size_t out_size);
/* Skip one JSON value (any type, nesting allowed). 0 ok, -1 malformed. */
int jskip_value(const char **pp);
/* Find "key": in a flat JSON object and return a pointer at the VALUE
 * start (the object's own top level only — nested objects are skipped
 * whole). NULL when absent or malformed. */
const char *jfind_value(const char *obj, const char *key);

#endif /* MINIJSON_H */
