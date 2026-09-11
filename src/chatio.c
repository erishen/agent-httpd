/* SSE transport primitives — see chatio.h. Extracted from llm.c so the
 * agent loop and future chat modules can emit events without owning the
 * socket protocol. */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

#include "chatio.h"
#include "minijson.h"

const char CHAT_SSE_HEAD[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: close\r\n"
    "\r\n";

int net_write_all(int fd, const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, buf + off, n - off, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

void sse_event(ChatOut *out, const char *type, const char *data) {
    sbuf b = {0};
    sb_str(&b, "data: {\"t\":\"");
    sb_str(&b, type);
    sb_str(&b, "\"");
    if (data) {
        sb_str(&b, ",\"d\":");
        sb_json_str(&b, data);
    }
    sb_str(&b, "}\n\n");
    if (!b.oom && out->ok && net_write_all(out->fd, b.p, b.len) == 0) {
        out->bytes += (int)b.len;
    } else {
        out->ok = 0;
    }
    /* session capture: mirror content deltas into the transcript buffer */
    if (!b.oom && data && strcmp(type, "delta") == 0 && out->cap_on) {
        sb_str(&out->cap, data);
    }
    free(b.p);
}
