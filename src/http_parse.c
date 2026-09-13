/* Request parsing: turn a raw HTTP request buffer into a bounded HttpRequest.
 * Every setter is the project-wide set_str, so over-long header values are
 * truncated-and-NUL-terminated rather than left unterminated. This module is
 * header-only for the request side — response serialization lives in
 * src/http_resp.c, the keep-alive connection loop in src/http.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "internal.h"
#include "httpd.h"

/* Case-insensitive substring search (ASCII), used by Transfer-Encoding below
 * and kept here with the parsing helpers. */
static const char *find_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) return p;
    }
    return NULL;
}

/* Header values go through the project-wide bounded setter (set_str): the
 * destination is a fixed-size field and is always NUL-terminated, so an
 * over-long header cannot leave a field unterminated. */
static void parse_header_line(const char *line, HttpRequest *request) {
    if (strncasecmp(line, "Host:", 5) == 0) {
        set_str(request->host, sizeof(request->host), line + 5);
        trim_whitespace(request->host);
    } else if (strncasecmp(line, "User-Agent:", 11) == 0) {
        set_str(request->user_agent, sizeof(request->user_agent), line + 11);
        trim_whitespace(request->user_agent);
    } else if (strncasecmp(line, "Referer:", 8) == 0) {
        set_str(request->referer, sizeof(request->referer), line + 8);
        trim_whitespace(request->referer);
    } else if (strncasecmp(line, "Origin:", 7) == 0) {
        set_str(request->origin, sizeof(request->origin), line + 7);
        trim_whitespace(request->origin);
    } else if (strncasecmp(line, "Accept:", 7) == 0) {
        set_str(request->accept, sizeof(request->accept), line + 7);
        trim_whitespace(request->accept);
    } else if (strncasecmp(line, "Accept-Encoding:", 16) == 0) {
        set_str(request->accept_encoding, sizeof(request->accept_encoding), line + 16);
        trim_whitespace(request->accept_encoding);
    } else if (strncasecmp(line, "Content-Type:", 13) == 0) {
        set_str(request->content_type, sizeof(request->content_type), line + 13);
        trim_whitespace(request->content_type);
    } else if (strncasecmp(line, "Connection:", 11) == 0) {
        set_str(request->connection, sizeof(request->connection), line + 11);
        trim_whitespace(request->connection);
    } else if (strncasecmp(line, "Authorization:", 14) == 0) {
        set_str(request->authorization, sizeof(request->authorization), line + 14);
        trim_whitespace(request->authorization);
    } else if (strncasecmp(line, "If-None-Match:", 14) == 0) {
        set_str(request->if_none_match, sizeof(request->if_none_match), line + 14);
        trim_whitespace(request->if_none_match);
    } else if (strncasecmp(line, "If-Modified-Since:", 18) == 0) {
        set_str(request->if_modified_since, sizeof(request->if_modified_since), line + 18);
        trim_whitespace(request->if_modified_since);
    } else if (strncasecmp(line, "Expect:", 7) == 0) {
        set_str(request->expect, sizeof(request->expect), line + 7);
        trim_whitespace(request->expect);
    } else if (strncasecmp(line, "Range:", 6) == 0) {
        set_str(request->range, sizeof(request->range), line + 6);
        trim_whitespace(request->range);
    } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
        long cl = strtol(line + 15, NULL, 10);
        request->content_length = (cl > 0 && cl <= MAX_REQUEST_SIZE) ? (int)cl : 0;
    } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
        /* RFC 9110 8.7: only chunked is defined for requests; a TE naming
         * any other (final) coding cannot be framed by this server. If TE
         * and Content-Length coexist, RFC 9110 6.1 says the message is so
         * poisoned it MUST be rejected (smuggling ambiguity) — handled by
         * the caller, which sees both flags. */
        const char *v = line + 18;
        if (find_ci(v, "chunked") && !find_ci(v, "gzip") &&
            !find_ci(v, "deflate") && !find_ci(v, "compress") &&
            !find_ci(v, "identity")) {
            request->chunked = 1;
        } else {
            /* some other transfer coding on a request: mark it so the body
             * read can answer 501 instead of framing garbage */
            request->te_unsupported = 1;
        }
    }
}

/* Parse the start line and headers, bounded by the buffer's length. Returns
 * the byte offset just past the "\r\n\r\n" (start of body) on success, or -1
 * on a malformed request. Never writes into the body region. Callers keep
 * the raw buffer for the body (see read_into_body). */
static long parse_headers(const char *raw_request, long len, HttpRequest *request) {
    long p = 0;
    /* request line */
    while (p < len && raw_request[p] != '\r' && raw_request[p] != '\n') p++;
    if (p == 0 || p >= len) return -1;
    char reqline[MAX_PATH_SIZE + 32];
    size_t rl = (size_t)(p < MAX_PATH_SIZE ? p : MAX_PATH_SIZE);
    memcpy(reqline, raw_request, rl);
    reqline[rl] = '\0';
    if (sscanf(reqline, "%15s %1023s %15s",
               request->method, request->path, request->protocol) != 3) {
        return -1;
    }

    /* header lines */
    for (;;) {
        if (p >= len) return -1;
        /* end of a line: consume its terminator (\r\n or bare \n) */
        if (raw_request[p] == '\r') {
            p++;
            if (p < len && raw_request[p] == '\n') p++;
        } else if (raw_request[p] == '\n') {
            p++;
        } else {
            long start = p;
            while (p < len && raw_request[p] != '\r' && raw_request[p] != '\n') p++;
            if (p == len) return -1;
            char header[MAX_PATH_SIZE];
            /* clamp in long arithmetic: mixing the two operands of a ?: across
             * signed/unsigned promotes the whole expression and trips GCC's
             * -Wsign-compare (fatal under -Werror) */
            long hlen = p - start;
            if (hlen > (long)sizeof header - 1) hlen = (long)sizeof header - 1;
            size_t hl = (size_t)hlen;
            memcpy(header, raw_request + start, hl);
            header[hl] = '\0';
            parse_header_line(header, request);
            continue; /* re-enter at the terminator we just found */
        }
        /* blank line (right after the terminator) closes the header block */
        if (p < len && (raw_request[p] == '\r' || raw_request[p] == '\n')) {
            if (raw_request[p] == '\r' && p + 1 < len && raw_request[p + 1] == '\n') p++;
            p++;
            return p; /* p now points at the body (or the buffer end) */
        }
    }
}

int parse_request(const char *raw_request, HttpRequest *request) {
    memset(request, 0, sizeof(HttpRequest));
    long len = strnlen(raw_request, MAX_REQUEST_SIZE);
    return parse_headers(raw_request, len, request) < 0 ? -1 : 0;
}
