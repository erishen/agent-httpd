#ifndef CHATIO_H
#define CHATIO_H

#include "minijson.h"

/* SSE transport primitives shared by the chat modules: the per-connection
 * output state and the event emitter. The envelope contract lives in the
 * node backend (react-ssr/server/chat.ts) and is mirrored verbatim:
 *   data: {"t":"delta","d":"<text>"}\n\n   incremental tokens
 *   data: {"t":"note","d":"<text>"}\n\n    one-off status line (tool traces
 *                                          ride this today — the page
 *                                          ignores unknown event types)
 *   data: {"t":"error","d":"<text>"}\n\n   fatal, stream ends after this
 *   data: {"t":"done"}\n\n                 terminal marker */

typedef struct {
    int fd;
    int ok;    /* 0 once the client is gone (send failed) */
    int bytes; /* head + events written (feeds the access log) */
    int cap_on;/* 1 to capture streamed "delta" text into cap */
    sbuf cap;  /* session transcript capture buffer (cap_on gates it) */
} ChatOut;

/* SSE response head, written once by llm_handle_chat before any events. */
extern const char CHAT_SSE_HEAD[];

/* Emit one SSE event; flips out->ok on write failure. data may be NULL
 * for the parameter-less forms ({"t":"done"}). */
void sse_event(ChatOut *out, const char *type, const char *data);

/* Full-write helpers: -1 when the peer is gone (SIGPIPE is ignored
 * server-wide; a vanished client just fails the send). */
int net_write_all(int fd, const char *buf, size_t n);

#endif /* CHATIO_H */
