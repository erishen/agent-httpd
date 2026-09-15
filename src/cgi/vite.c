/* src/cgi/vite.c - Vite dev-proxy header construction, extracted from http.c
 * for testability. Self-contained: depends only on the C standard library
 * (strncasecmp comes from <strings.h>). */
#include <string.h>
#include <strings.h>
#include <stdio.h>

/* Reject a header line carrying CR or LF anywhere except its own CRLF
 * terminator - a request-smuggling vector. Returns 1 when such a byte is
 * found before the trailing CRLF. */
int line_has_embedded_crlf(const char *line, size_t ll) {
    if (ll < 2) return 0; /* no room for a CRLF terminator */
    size_t last = ll - 2; /* bytes before the trailing CRLF */
    for (size_t i = 0; i < last; i++) {
        if (line[i] == '\r' || line[i] == '\n') return 1;
    }
    return 0;
}

/* Build the upstream (Vite) request header block into buf[bufsz].
 * `raw` is the client's raw header block (hdr_len bytes, including the
 * terminating blank line); the request line is NOT copied here - the caller
 * forwards it verbatim first. Hop-by-hop headers we own are dropped, then a
 * Host/Connection tail is appended. Returns the number of bytes written,
 * which is always <= bufsz (the caller's send() never reads OOB). */
size_t vite_build_headers(char *buf, size_t bufsz, const char *raw,
                           size_t hdr_len, int port) {
    static const char *const strip_pfx[] = {
        "Host:", "Connection:", "Transfer-Encoding:", "TE:",
        "Keep-Alive:", "Proxy-Connection:", "Proxy-Authenticate:",
        "Proxy-Authorization:", "Upgrade:", "Trailer:", "Expect:", NULL,
    };
    const char *nl = memchr(raw, '\n', hdr_len);
    size_t first_len = nl ? (size_t)(nl - raw + 1) : hdr_len;
    size_t off = first_len;
    size_t blen = 0;
    while (off < hdr_len) {
        const char *ln = memchr(raw + off, '\n', hdr_len - off);
        size_t ll = ln ? (size_t)(ln - (raw + off) + 1) : hdr_len - off;
        const char *line = raw + off;
        /* blank line = end of the header block; stop before it */
        if (ll == 1 || (ll == 2 && line[0] == '\r')) break;
        int drop = 0;
        for (int i = 0; strip_pfx[i]; i++) {
            if (strncasecmp(line, strip_pfx[i], strlen(strip_pfx[i])) == 0) {
                drop = 1;
                break;
            }
        }
        /* a header value with an embedded CRLF is a smuggled second request
         * aimed at the upstream Vite; do not forward it (caller 400s). */
        if (!drop && line_has_embedded_crlf(line, ll)) break;
        /* keep >=64 bytes of headroom so the trailing Host/Connection line
         * can never push blen past the buffer (a 16KB header block is
         * pathological; we simply stop forwarding extra non-hop-by-hop lines). */
        if (!drop && blen + ll + 64 <= bufsz) {
            memcpy(buf + blen, line, ll);
            blen += ll;
        }
        off += ll;
    }
    int extra = snprintf(buf + blen, bufsz - blen,
                         "Host: 127.0.0.1:%d\r\nConnection: close\r\n\r\n",
                         port);
    if (extra < 0) extra = 0;
    if (blen + (size_t)extra > bufsz)
        extra = (int)(bufsz - blen); /* clamp: never read OOB in send */
    blen += (size_t)extra;
    return blen;
}
