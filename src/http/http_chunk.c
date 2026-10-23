/* Chunked transfer coding decoder (RFC 9110 8.7.1), factored out of the
 * request-body read loop (src/http/http.c) so the framing logic is a pure,
 * fuzzable state machine: it does no I/O, no timeouts and no allocation.
 *
 * The caller feeds wire bytes (the undecoded body region, monotonically
 * growing) through http_dechunk_feed(); the decoded payload is written
 * contiguously to out (bounded by cap). Return codes:
 *   1  need more data (caller should recv more and feed again);
 *   0  stream fully decoded (trailing 0-chunk + trailer block consumed);
 *  -1  malformed framing (400-class);
 *  -2  decoded body exceeds cap (413-class).
 *
 * Chunk grammar handled: "hex-size[;ext]CRLF" data CRLF, 0-chunk with
 * optional trailer block up to the blank line. Semantics mirror the
 * original in-place loop in http.c exactly (strtol base-16 parsing, lone
 * line or CRLF line ends the trailer block, payload+CRLF consumed per
 * chunk); only the recv/select/timeout plumbing moved to the caller. */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "httpd.h"

/* Find the next '\n' at or after p; returns its index, or -1. */
static long find_lf(const char *wire, long wire_len, long p) {
    for (long i = p; i < wire_len; i++) {
        if (wire[i] == '\n') return i;
    }
    return -1;
}

int http_dechunk_feed(const char *wire, long wire_len, void *state,
                      char *out, long cap, long *out_len) {
    HttpChunkState *st = (HttpChunkState *)state;
    if (st->phase == 3) {
        if (out_len) *out_len = st->written;
        return 0;
    }

    for (;;) {
        if (st->phase == 0) {
            /* chunk-size line: "HEX[;ext]" up to LF at ls (RFC 9110 8.7.1).
             * The size is parsed with an explicit [st->p, ls) bound: the
             * former strtol(wire + st->p, NULL, 16) ignored wire_len and
             * over-read past the input (heap-buffer-overflow caught by the
             * fuzzer under ASan). Bounding to ls removes the over-read while
             * keeping behaviour identical for valid inputs (hex digits, the
             * ';' extension separator, leading whitespace and an optional
             * sign are all handled exactly as strtol did). */
            long ls = find_lf(wire, wire_len, st->p);
            if (ls < 0) { if (out_len) *out_len = st->written; return 1; }
            long j = st->p;
            while (j < ls && (wire[j] == ' ' || wire[j] == '\t')) j++;
            int neg = 0;
            if (j < ls && wire[j] == '+') j++;
            else if (j < ls && wire[j] == '-') { neg = 1; j++; }
            long sz = 0;
            int digits = 0;
            while (j < ls) {
                char d = wire[j];
                int v;
                if (d >= '0' && d <= '9') v = d - '0';
                else if (d >= 'a' && d <= 'f') v = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F') v = d - 'A' + 10;
                else break;              /* ';' extension or stray -> stop */
                if (sz > (LONG_MAX - v) / 16) {  /* would overflow long:
                                          * malformed, and it also keeps the
                                          * written+sz cap check below
                                          * overflow-free (UBSan-clean) */
                    if (out_len) *out_len = st->written;
                    return -1;
                }
                sz = sz * 16 + v;
                if (++digits > 16) {     /* absurd size: malformed */
                    if (out_len) *out_len = st->written;
                    return -1;
                }
                j++;
            }
            if (neg) sz = -sz;
            st->p = ls + 1;
            if (sz < 0) { if (out_len) *out_len = st->written; return -1; }
            if (sz == 0) { st->phase = 2; continue; }
            if (st->written + sz > cap) { if (out_len) *out_len = st->written; return -2; }
            st->pending = sz;
            st->phase = 1;
            continue;
        }

        if (st->phase == 1) {
            /* payload + trailing CRLF; like the original streaming loop,
             * wait until the full chunk (data + CRLF) is available before
             * consuming — partial consumption would leak CRLF bytes into
             * the next chunk-size parse. */
            long avail = wire_len - st->p;
            long need = st->pending + 2;
            if (avail < need) { if (out_len) *out_len = st->written; return 1; }
            memcpy(out + st->written, wire + st->p, (size_t)st->pending);
            st->written += st->pending;
            st->p += need; /* skip payload + CRLF */
            st->pending = 0;
            st->phase = 0;
            continue;
        }

        if (st->phase == 2) {
            /* trailer block: lines up to the blank line */
            long ts = find_lf(wire, wire_len, st->p);
            if (ts < 0) { if (out_len) *out_len = st->written; return 1; }
            if (ts == st->p || (ts == st->p + 1 && wire[st->p] == '\r')) {
                st->phase = 3;
                if (out_len) *out_len = st->written;
                return 0; /* blank line ends the stream */
            }
            st->p = ts + 1;
            continue;
        }
    }
}

/* Convenience wrapper for fully-buffered inputs (tests, fuzzer): decode one
 * shot, returns same codes as http_dechunk_feed. */
int http_dechunk(const char *wire, long wire_len, char *out, long cap,
                 long *out_len) {
    HttpChunkState st;
    memset(&st, 0, sizeof st);
    return http_dechunk_feed(wire, wire_len, &st, out, cap, out_len);
}
