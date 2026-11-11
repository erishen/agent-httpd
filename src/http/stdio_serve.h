#ifndef STDIO_SERVE_H
#define STDIO_SERVE_H

/* stdio transport — a socket-free alternative to the HTTP listener.
 *
 * Why it exists: an app that drives the agent as a child process (parent
 * writes requests to the child's stdin, reads frames from its stdout) has no
 * use for a TCP port. A listener costs a port number, a port-collision
 * dance, and — above all — a whole class of "Connection refused" failures
 * that exist only because the transport happens to be a socket.
 *
 * The transport is the SAME router the HTTP listener uses: a stdio request
 * is turned into an HttpRequest and handed to the same dispatch chain, so
 * every route a .lume script registered answers over a pipe exactly as it
 * does over TCP. Nothing about the routes changes; only the carrier does.
 *
 * Wire format
 * -----------
 * request (stdin, one JSON object per line, newline-terminated):
 *
 *   {"method":"POST","path":"/api/energy/telemetry","query":"a=1",
 *    "body":{"sentAt":"...","telemetry":{...}}}
 *
 *   method  optional, defaults to GET
 *   path    required
 *   query   optional raw query string, without the leading '?'
 *   body    optional; a JSON object/array is used verbatim as the request
 *           body, a JSON string is unescaped and used as the raw body text.
 *           Absent means an empty body (never NULL: the form/JSON handlers
 *           dereference request->body).
 *
 * response (stdout, SSE frames — the same envelope the HTTP chat route
 * uses, so one client-side parser serves both transports):
 *
 *   data: {"t":"result","status":200,"d":"<body>"}\n\n
 *   data: {"t":"done"}\n\n
 *
 *   "d" is always a JSON string. A route body is itself JSON, but not every
 *   route produces JSON (/health answers text/plain), so the payload is
 *   carried as a string and the client parses it once more when it wants the
 *   object. "status" is the HTTP status the route produced, kept so the
 *   client can tell 503 from 200 without guessing.
 *
 *   The chat route (/react/api/chat, /react/api/pse) is different: it
 *   streams its own frames (delta / note / error) and terminates with its
 *   own "done" frame, because those frames are the response.
 *
 * stdout belongs to the protocol: the loop dups it and points the process'
 * stdout at stderr, so a .lume script's print() lands on the supervisor's
 * log instead of corrupting the frame stream.
 */

/* 1 while the process speaks the stdio transport. Set by the stdio loop
 * before any handler can run; llm_handle_chat reads it to skip the HTTP
 * response head (a pipe has no status line). */
extern int g_stdio_mode;

/* Claim stdout for the protocol: dup it, then point the process' stdout at
 * stderr so nothing else can write to the frame stream (a .lume script's
 * print(), the framework's own diagnostics). Returns the fd that carries
 * the protocol. Call this FIRST — before any startup diagnostic is
 * printed — otherwise those lines land on the protocol stream. */
int stdio_claim_stdout(void);

/* Run the stdio request loop until stdin reaches EOF (the parent closed the
 * pipe) or the peer disappears. Returns 0 on a clean end of input. */
int agenthttpd_stdio_serve(void);

#endif /* STDIO_SERVE_H */
