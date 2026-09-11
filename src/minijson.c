/* Dependency-free JSON helpers — see minijson.h. Extracted from llm.c when
 * the chat endpoint grew into the agent modules (tools/agent also need the
 * same builder + tolerant reader). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "minijson.h"

/* ---- growable string buffer -------------------------------------- */

static void sb_reserve(sbuf *b, size_t extra) {
    if (b->oom) return;
    if (b->p && b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    char *n = realloc(b->p, cap);
    if (!n) {
        b->oom = 1;
        return;
    }
    b->p = n;
    b->cap = cap;
    if (b->len == 0) b->p[0] = '\0';
}

void sb_mem(sbuf *b, const char *s, size_t n) {
    sb_reserve(b, n);
    if (b->oom) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void sb_str(sbuf *b, const char *s) {
    sb_mem(b, s, strlen(s));
}

void sb_chr(sbuf *b, char c) {
    sb_mem(b, &c, 1);
}

void sb_json_str(sbuf *b, const char *s) {
    sb_chr(b, '"');
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) {
        unsigned char c = *q;
        switch (c) {
        case '"':  sb_str(b, "\\\""); break;
        case '\\': sb_str(b, "\\\\"); break;
        case '\b': sb_str(b, "\\b"); break;
        case '\f': sb_str(b, "\\f"); break;
        case '\n': sb_str(b, "\\n"); break;
        case '\r': sb_str(b, "\\r"); break;
        case '\t': sb_str(b, "\\t"); break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", c);
                sb_str(b, tmp);
            } else {
                sb_chr(b, (char)c);
            }
        }
    }
    sb_chr(b, '"');
}

/* ---- tolerant reader ---------------------------------------------- */

const char *jws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static unsigned hex4(const char *q) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = q[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else v |= (unsigned)(c - 'A' + 10);
    }
    return v;
}

/* Append one codepoint as UTF-8, truncating at out_size-1. Only escape
 * results take this path — non-escape bytes bypass it so multi-byte UTF-8
 * sequences are never re-encoded. */
static void utf8_put(char *out, size_t out_size, size_t *n, unsigned cp) {
    char tmp[4];
    int k;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        k = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        k = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        k = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        k = 4;
    }
    for (int i = 0; i < k; i++) {
        if (*n + 1 < out_size) out[(*n)++] = tmp[i];
    }
}

const char *jread_string(const char **pp, char *out, size_t out_size) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p != '"') {
        if (*p == '\0') return NULL;
        if (*p == '\\') {
            unsigned cp;
            p++;
            switch (*p) {
            case '"':  cp = '"';  break;
            case '\\': cp = '\\'; break;
            case '/':  cp = '/';  break;
            case 'b':  cp = '\b'; break;
            case 'f':  cp = '\f'; break;
            case 'n':  cp = '\n'; break;
            case 'r':  cp = '\r'; break;
            case 't':  cp = '\t'; break;
            case 'u': {
                if (!isxdigit((unsigned char)p[1]) ||
                    !isxdigit((unsigned char)p[2]) ||
                    !isxdigit((unsigned char)p[3]) ||
                    !isxdigit((unsigned char)p[4]))
                    return NULL;
                cp = hex4(p + 1);
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    p[1] == '\\' && p[2] == 'u' &&
                    isxdigit((unsigned char)p[3]) &&
                    isxdigit((unsigned char)p[4]) &&
                    isxdigit((unsigned char)p[5]) &&
                    isxdigit((unsigned char)p[6])) {
                    unsigned lo = hex4(p + 3);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                break;
            }
            default:
                return NULL;
            }
            p++;
            utf8_put(out, out_size, &n, cp);
        } else {
            /* Non-escape bytes pass through verbatim: UTF-8 sequences are
             * copied byte-for-byte. Decoding them as single codepoints
             * would re-encode every continuation byte and mangle all
             * non-ASCII text (e.g. Chinese) in both directions. */
            if (n + 1 < out_size) out[n++] = *p;
            p++;
        }
    }
    out[n] = '\0';
    p++;
    *pp = p;
    return p;
}

int jskip_value(const char **pp) {
    const char *p = jws(*pp);
    if (*p == '"') {
        char scratch[8];
        if (!jread_string(&p, scratch, sizeof scratch)) return -1;
    } else if (*p == '{' || *p == '[') {
        char open_c = *p, close_c = (*p == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                char scratch[8];
                if (!jread_string(&p, scratch, sizeof scratch)) return -1;
                continue;
            }
            if (*p == open_c) {
                depth++;
            } else if (*p == close_c) {
                depth--;
                if (depth == 0) {
                    p++;
                    break;
                }
            }
            p++;
        }
        if (depth != 0) return -1;
    } else {
        const char *start = p;
        while (*p && *p != ',' && *p != '}' && *p != ']') p++;
        if (p == start) return -1;
    }
    *pp = p;
    return 0;
}

const char *jfind_value(const char *obj, const char *key) {
    const char *p = jws(obj);
    if (*p != '{') return NULL;
    p++;
    for (;;) {
        p = jws(p);
        if (*p == '}' || *p == '\0') return NULL;
        char kbuf[64];
        if (!jread_string(&p, kbuf, sizeof kbuf)) return NULL;
        p = jws(p);
        if (*p != ':') return NULL;
        p = jws(p + 1);
        if (strcmp(kbuf, key) == 0) return p;
        if (jskip_value(&p) != 0) return NULL;
        p = jws(p);
        if (*p == ',') {
            p++;
        } else {
            return NULL;
        }
    }
}
