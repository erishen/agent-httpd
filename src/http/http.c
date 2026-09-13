/* HTTP connection core: the listening socket (create_server_socket), the
 * lifecycle signal handler, the dev-mode Vite relay (proxy_to_vite /
 * ws_tunnel), and above all the keep-alive connection loop (handle_client)
 * that parses a request, reads its body, dispatches it, serializes the
 * response and streams it out. Request parsing, response serialization,
 * access logging and route classification live in their own modules
 * (src/http_parse.c, src/http_resp.c, src/http_log.c, src/http_route.c). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <fcntl.h>
#ifdef __linux__
#include <sys/sendfile.h> /* sendfile(2) */
#endif

#include "internal.h"
#include "metrics.h"
#include "llm.h"

volatile sig_atomic_t g_server_running = 1;
volatile sig_atomic_t g_reopen_log = 0;
volatile sig_atomic_t g_resync = 0;
FILE *g_log_fp = NULL;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_server_running = 0;
    } else if (sig == SIGHUP) {
        g_reopen_log = 1;
        g_resync = 1;
    }
}

/* Send the interim 100 Continue response (best effort). Called right
 * before a request body is read from the socket, when the client asked
 * for it via Expect: 100-continue. */
static void send_continue(int client_fd) {
    if (client_fd < 0) return;
    static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
    ssize_t w = send(client_fd, cont, sizeof(cont) - 1, 0);
    (void)w; /* SIGPIPE ignored; a vanished client just won't send the body */
}

int create_server_socket(int port) {
    int server_fd;
    struct sockaddr_in server_addr;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return -1;
    }
#ifdef __linux__
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEPORT)");
    }
#endif

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

/* Dev mode: forward one request to the internal Vite server (g_vite_upstream
 * _port) and stream its response back to the client. Used for client module
 * transforms and dev SSR pages — everything the C server cannot render
 * itself. Runs on a blocking pool worker; the header block is replayed
 * verbatim (minus Host/Connection, which we control). */
/* A smuggled request hides inside a header value as an embedded CRLF
 * (e.g. "Foo: bar\r\nGET /x HTTP/1.1\r\n"). We are the framing authority, so
 * reject any header line that carries a CR/LF anywhere except its own
 * CRLF terminator (the last two bytes). */
static void proxy_to_vite(int client_fd, const char *raw, size_t hdr_len,
                          const HttpRequest *req) {
    int up = socket(AF_INET, SOCK_STREAM, 0);
    if (up < 0) {
        close(client_fd);
        return;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    sa.sin_port = htons((uint16_t)g_vite_upstream_port);
    if (connect(up, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        const char *err = "HTTP/1.1 502 Bad Gateway\r\n"
                          "Content-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(client_fd, err, (int)strlen(err), 0);
        close(up);
        close(client_fd);
        return;
    }

    /* request line (verbatim, keeps the query string) */
    const char *nl = memchr(raw, '\n', hdr_len);
    size_t first_len = nl ? (size_t)(nl - raw + 1) : hdr_len;
    /* reject a smuggled request line (embedded CRLF before its terminator) */
    if (line_has_embedded_crlf(raw, first_len)) {
        const char *err = "HTTP/1.1 400 Bad Request\r\n"
                          "Content-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(client_fd, err, (int)strlen(err), 0);
        close(up);
        close(client_fd);
        return;
    }
    if (send(up, raw, first_len, 0) < 0) goto done;

    /* remaining header lines, dropping hop-by-hop headers we control.
     * Beyond Host/Connection, stripping Transfer-Encoding, TE, Keep-Alive,
     * Proxy-*, Upgrade and Trailer is not just RFC 9110 7.6.1 hygiene: a
     * client smuggling a Transfer-Encoding header alongside our
     * Content-Length would desync the upstream's framing (request
     * smuggling). We are the framing authority now: the body is always
     * sent with an exact Content-Length. */
    char buf[16384];
    size_t n = vite_build_headers(buf, sizeof(buf), raw, hdr_len, g_vite_upstream_port);
    if (send(up, buf, (int)n, 0) < 0) goto done;
    if (req->body && req->content_length > 0) {
        size_t left = (size_t)req->content_length;
        const char *bp = req->body;
        while (left > 0) {
            ssize_t w = send(up, bp, left, 0);
            if (w <= 0) goto done;
            bp += w;
            left -= (size_t)w;
        }
    }

    /* stream the upstream response straight back */
    {
        char rbuf[65536];
        for (;;) {
            ssize_t r = recv(up, rbuf, sizeof(rbuf), 0);
            if (r <= 0) break;
            const char *sp = rbuf;
            ssize_t left = r;
            while (left > 0) {
                ssize_t w = send(client_fd, sp, (size_t)left, 0);
                if (w <= 0) { left = 0; break; }
                sp += w;
                left -= w;
            }
        }
    }
done:
    close(up);
    close(client_fd);
}

/* Dev mode: WebSocket upgrade tunnel for Vite HMR. The raw header block
 * (with Sec-WebSocket-Key) is replayed to the internal Vite server, then the
 * two sockets are spliced byte-for-byte until either side closes. */
static void ws_tunnel(int client_fd, const char *raw, size_t hdr_len) {
    int up = socket(AF_INET, SOCK_STREAM, 0);
    if (up < 0) {
        close(client_fd);
        return;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    sa.sin_port = htons((uint16_t)g_vite_upstream_port);
    if (connect(up, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(up);
        close(client_fd);
        return;
    }
    if (send(up, raw, hdr_len, 0) < 0) {
        close(up);
        close(client_fd);
        return;
    }

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        FD_SET(up, &rfds);
        int hi = client_fd > up ? client_fd : up;
        if (select(hi + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char b[65536];
        int from = -1, to = -1;
        if (FD_ISSET(client_fd, &rfds)) { from = client_fd; to = up; }
        else if (FD_ISSET(up, &rfds)) { from = up; to = client_fd; }
        if (from < 0) continue;
        ssize_t n = recv(from, b, sizeof(b), 0);
        if (n <= 0) break;
        const char *sp = b;
        ssize_t left = n;
        while (left > 0) {
            ssize_t w = send(to, sp, (size_t)left, 0);
            if (w <= 0) { left = 0; break; }
            sp += w;
            left -= w;
        }
    }
    close(up);
    close(client_fd);
}

void handle_client(int client_fd, struct sockaddr_in *client_addr) {
    char buffer[MAX_REQUEST_SIZE];
    char response_buffer[MAX_RESPONSE_SIZE];
    const char *client_ip = client_addr ? inet_ntoa(client_addr->sin_addr) : "-";
    int req_to = get_request_timeout();

    /* Every handle_client invocation is by definition off the fast path
     * (a pool worker took the fd, or fork mode forked for it). */
    METRICS_INC_AT(requests_total, 1);

    /* Responses are header block + body in separate sends; TCP_NODELAY keeps
     * Nagle from batching the first tiny write against the delayed-ACK timer
     * (latency spikes of tens of ms on the first byte of every response).
     * Idempotent: harmless if the dispatcher already set it. */
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* This handler does blocking I/O: every send loop below (the in-buffer
     * send(), the sendfile streamer, the CGI/FastCGI relays) treats a short
     * write as fatal. The event loop accepts with O_NONBLOCK because *it*
     * needs that for epoll/kqueue, and hands the very same socket to a pool
     * worker over SCM_RIGHTS - so the flag is still set here. Without this
     * clear, a body larger than the socket send buffer fails with EAGAIN
     * mid-stream, the response is abandoned under a Content-Length that
     * promised all of it, and the client sees a truncated file (from nginx:
     * "upstream prematurely closed connection"; from a browser: a JS bundle
     * that never parses, so the page silently never hydrates). A fast local
     * reader rarely fills the buffer, which is why it looked fine. */
    {
        int fl = fcntl(client_fd, F_GETFL, 0);
        if (fl != -1 && (fl & O_NONBLOCK)) {
            (void)fcntl(client_fd, F_SETFL, fl & ~O_NONBLOCK);
        }
    }

    /* HTTP/1.1 persistent-connection loop (RFC 7230 section 6.3): serve
     * requests back-to-back until the client asks to close, the request or
     * response cap is hit, an error/timeout strikes, or the response length
     * is unknown (streamed relay) - then close. */
    int carry = 0;                  /* bytes of the next pipelined request
                                     * still parked in buffer from a prior
                                     * request (HTTP/1.1 keep-alive pipelining) */
    long prev_consumed = 0;         /* end offset of the last fully-parsed
                                     * request within buffer */
    for (int served = 0;;) {
    HttpRequest request;
    HttpResponse response;
    int total = 0;
    int header_end_len = 0;
    int keep_alive_force_close = 0; /* set by the 401 challenge path */
    int headers_oversized = 0;      /* header block exceeded the buffer */

    if (carry > 0) {
        /* A previous request left bytes of the next pipelined request in
         * buffer. Shift them to the front and keep parsing from there
         * instead of discarding them: the old code zeroed the buffer and
         * re-recv'd, which silently dropped any pipelined request that had
         * already arrived (the client would hang until its timeout). */
        memmove(buffer, buffer + prev_consumed, (size_t)carry);
        total = carry;
        buffer[total] = '\0';
        memset(buffer + total, 0, sizeof(buffer) - (size_t)total);
        carry = 0;
    } else {
        memset(buffer, 0, sizeof(buffer));
    }
    memset(&response, 0, sizeof(response));
    memset(&request, 0, sizeof(request));
    snprintf(request.remote_addr, sizeof(request.remote_addr), "%s", client_ip);

    /* Header read with an overall timeout: a client that dribbles bytes
     * (Slowloris-style) must not pin this forked handler forever. The first
     * request on a connection gets the full request timeout; idle waits for
     * subsequent keep-alive requests use the shorter KEEPALIVE_TIMEOUT. */
    int wait_to = (served == 0) ? req_to : KEEPALIVE_TIMEOUT_SECONDS;
    time_t deadline = wait_to > 0 ? time(NULL) + wait_to : 0;
    /* Parse from what is already buffered before waiting for more: a
     * carried-over pipelined request is complete in `buffer` and no further
     * bytes will ever arrive for it (the client is waiting for our
     * response), so blocking on select()/recv() first parks it until the
     * keep-alive timeout and the client sees a swallowed request. */
    while (1) {
        char *header_end = strstr(buffer, "\r\n\r\n");
        if (header_end) {
            header_end_len = (header_end + 4) - buffer;
            break;
        }
        if (total >= (int)sizeof(buffer) - 1) {
            headers_oversized = 1; /* no \r\n\r\n and no room left */
            break;
        }
        if (deadline && time(NULL) >= deadline) {
            close(client_fd); /* overall request window spent */
            return;
        }
        struct timeval tv = {wait_to > 0 ? wait_to : 300, 0};
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        if (select(client_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            close(client_fd); /* timed out or interrupted waiting for headers */
            return;
        }
        int n = recv(client_fd, buffer + total, sizeof(buffer) - 1 - total, 0);
        if (n <= 0) {
            close(client_fd);
            return;
        }
        total += n;
        buffer[total] = '\0';
    }

    if (headers_oversized) {
        /* RFC 9110 9.5.6: the header block cannot be read as a whole.
         * Close afterwards - the framing state is unrecoverable. */
        set_error_response(&response, 431, "Request Header Fields Too Large");
        keep_alive_force_close = 1;
    } else if (parse_request(buffer, &request) < 0) {
        set_error_response(&response, 400, "Bad Request");
    } else if (deadline && time(NULL) >= deadline) {
        /* headers arrived, but the overall request window is already spent */
        set_error_response(&response, 408, "Request Timeout");
    } else if (g_rate_limit_rps > 0 &&
               !rate_limit_allow(client_addr ? client_addr->sin_addr.s_addr : 0)) {
        /* Checked before auth on purpose: a flood must not reach dispatch or
         * burn crypt() CPU. 429 + Retry-After, then close (queued requests
         * from the same abuser would otherwise smuggle past the counter). */
        set_error_response(&response, 429, "Too Many Requests");
        response.retry_after = 1;
        keep_alive_force_close = 1;
    } else if (!check_basic_auth(request.authorization)) {
        /* 401 challenge. The relay path above answers only when the backend
         * replied; auth runs first, so the backend never sees
         * unauthenticated traffic either. After a 401 the connection is not
         * reusable with certainty (pipelined body state), so close it. */
        set_error_response(&response, 401, "Unauthorized");
        response.auth_realm = g_auth_realm;
        keep_alive_force_close = 1;        } else {
            /* Expect: 100-continue (RFC 9110 10.1.1): the client holds the
             * body until granted. The stall happens in the body read below,
             * so grant here - just before it, and only when a body is
             * actually expected and not already fully buffered (no continue
             * for 413: the client gets the final status instead). */
            if ((request.content_length > 0 || request.chunked) &&
                strncasecmp(request.expect, "100-continue", 12) == 0 &&
                response.status_code == 0) {
                /* chunked never arrives "fully buffered" (the size lines
                 * keep coming), so it always grants the continue; the CL
                 * path keeps its not-already-buffered condition */
                if (request.chunked || total - header_end_len < request.content_length) {
                    send_continue(client_fd);
                }
            }
            /* Framing sanity first (RFC 9110 6.1 / 8.7): a request that
             * names both Content-Length and Transfer-Encoding is unframeable
             * (classic smuggling shape) — reject with 400. A request TE we
             * don't implement (anything but the final "chunked") gets 501.
             * Both close the connection: the framing state is poisoned. */
            if (request.chunked && request.content_length > 0) {
                set_error_response(&response, 400, "Bad Request");
                keep_alive_force_close = 1;
            } else if (request.te_unsupported && !request.chunked) {
                set_error_response(&response, 501, "Not Implemented");
                keep_alive_force_close = 1;
            } else if (request.chunked) {
                /* RFC 9110 8.7.1: decode the chunked stream in place. The
                 * body is re-assembled contiguously in a fresh buffer (the
                 * raw wire bytes are chunk metadata + payload interleaved),
                 * capped at MAX_REQUEST_SIZE like the CL path. */
                char *body = malloc(MAX_REQUEST_SIZE + 1);
                int blen = 0, ok = 1;
                if (!body) {
                    set_error_response(&response, 500, "Internal Server Error");
                } else {
                    /* p walks the undecoded stream: what we already recv'd
                     * past the headers, plus whatever keeps arriving. */
                    long p = header_end_len;
                    int have = total;
                    for (;;) {
                        /* ensure a full line at p (a chunk-size line) */
                        long ls = p;
                        while (1) {
                            for (; ls < have; ls++) {
                                if (buffer[ls] == '\n') break;
                            }
                            if (ls < have) break;
                            if (deadline && time(NULL) >= deadline) {
                                set_error_response(&response, 408, "Request Timeout");
                                ok = 0;
                                goto chunk_done;
                            }
                            if (have >= (int)sizeof(buffer) - 1) {
                                set_error_response(&response, 431, "Request Header Fields Too Large");
                                ok = 0;
                                goto chunk_done;
                            }
                            struct timeval tv = {req_to > 0 ? req_to : 300, 0};
                            fd_set rfds;
                            FD_ZERO(&rfds);
                            FD_SET(client_fd, &rfds);
                            if (select(client_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
                                set_error_response(&response, 408, "Request Timeout");
                                ok = 0;
                                goto chunk_done;
                            }
                            int n = recv(client_fd, buffer + have, sizeof(buffer) - 1 - have, 0);
                            if (n <= 0) {
                                set_error_response(&response, 400, "Bad Request");
                                ok = 0;
                                goto chunk_done;
                            }
                            have += n;
                        }
                        /* parse "HEX[;ext]" up to the \n */
                        char *ls2 = buffer + p;
                        long sz = strtol(ls2, NULL, 16);
                        /* skip to just past the size line */
                        p = ls + 1;
                        if (sz < 0) {
                            set_error_response(&response, 400, "Bad Request");
                            ok = 0;
                            goto chunk_done;
                        }
                        if (sz == 0) {
                            /* terminating chunk; swallow the trailer block
                             * up to the blank line (RFC 9110 8.7.1). */
                            for (;;) {
                                long ts = p;
                                int done = 0;
                                while (1) {
                                    for (; ts < have; ts++) {
                                        if (buffer[ts] == '\n') {
                                            /* lone line = end of trailers */
                                            if ((ts == p) ||
                                                (ts == p + 1 && buffer[p] == '\r')) {
                                                done = 1;
                                            }
                                            break;
                                        }
                                    }
                                    if (done || ts < have) break;
                                    if (deadline && time(NULL) >= deadline) {
                                        set_error_response(&response, 408, "Request Timeout");
                                        ok = 0;
                                        goto chunk_done;
                                    }
                                    if (have >= (int)sizeof(buffer) - 1) {
                                        set_error_response(&response, 431, "Request Header Fields Too Large");
                                        ok = 0;
                                        goto chunk_done;
                                    }
                                    struct timeval tv = {req_to > 0 ? req_to : 300, 0};
                                    fd_set rfds;
                                    FD_ZERO(&rfds);
                                    FD_SET(client_fd, &rfds);
                                    if (select(client_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
                                        set_error_response(&response, 408, "Request Timeout");
                                        ok = 0;
                                        goto chunk_done;
                                    }
                                    int n = recv(client_fd, buffer + have, sizeof(buffer) - 1 - have, 0);
                                    if (n <= 0) {
                                        set_error_response(&response, 400, "Bad Request");
                                        ok = 0;
                                        goto chunk_done;
                                    }
                                    have += n;
                                }
                                if (done) break;
                                p = ts + 1;
                            }
                            break; /* chunked body complete */
                        }
                        /* read sz payload bytes + CRLF into the body */
                        if (blen + sz > MAX_REQUEST_SIZE) {
                            set_error_response(&response, 413, "Request Entity Too Large");
                            ok = 0;
                            goto chunk_done;
                        }
                        long need = sz + 2; /* data + CRLF */
                        while (have - p < need) {
                            if (deadline && time(NULL) >= deadline) {
                                set_error_response(&response, 408, "Request Timeout");
                                ok = 0;
                                goto chunk_done;
                            }
                            if (have >= (int)sizeof(buffer) - 1) {
                                set_error_response(&response, 413, "Request Entity Too Large");
                                ok = 0;
                                goto chunk_done;
                            }
                            struct timeval tv = {req_to > 0 ? req_to : 300, 0};
                            fd_set rfds;
                            FD_ZERO(&rfds);
                            FD_SET(client_fd, &rfds);
                            if (select(client_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
                                set_error_response(&response, 408, "Request Timeout");
                                ok = 0;
                                goto chunk_done;
                            }
                            int n = recv(client_fd, buffer + have, sizeof(buffer) - 1 - have, 0);
                            if (n <= 0) {
                                set_error_response(&response, 400, "Bad Request");
                                ok = 0;
                                goto chunk_done;
                            }
                            have += n;
                        }
                        memcpy(body + blen, buffer + p, (size_t)sz);
                        blen += (int)sz;
                        p += need;
                    }
                chunk_done:
                    if (ok) {
                        body[blen] = '\0';
                        request.body = body;
                        request.content_length = blen; /* downstream sees a
                            plain CL-framed body: CGI stdin, chat parse and
                            the FCGI relay are all untouched */
                    } else {
                        free(body);
                        keep_alive_force_close = 1;
                    }
                }
            } else if (request.body == NULL && response.status_code == 0) {
                int body_start = header_end_len;
                int have = total - body_start;
                if (have < request.content_length) {
                    int avail = (int)sizeof(buffer) - 1 - total;
                    if (request.content_length - have > avail) {
                        set_error_response(&response, 413, "Request Entity Too Large");
                        /* Body did not fit the single read buffer: the
                         * remaining bytes are still on the socket. If we
                         * kept the connection alive the client's trailing
                         * body would be parsed as the next request
                         * (keep-alive desync / request smuggling). Force a
                         * close so the client must resend on a fresh
                         * connection. */
                        keep_alive_force_close = 1;
                    } else {
                        while (have < request.content_length) {
                            if (deadline && time(NULL) >= deadline) {
                                set_error_response(&response, 408, "Request Timeout");
                                break;
                            }
                            struct timeval tv = {req_to > 0 ? req_to : 300, 0};
                            fd_set rfds;
                            FD_ZERO(&rfds);
                            FD_SET(client_fd, &rfds);
                            if (select(client_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
                                set_error_response(&response, 408, "Request Timeout");
                                break;
                            }
                            int n = recv(client_fd, buffer + total, sizeof(buffer) - 1 - total, 0);
                            if (n <= 0) break;
                            total += n;
                            have += n;
                        }
                    }
                }
                /* Runs for framed and body-less requests alike: the
                 * pipelining bookkeeping below is what carries a subsequent
                 * request forward, so a zero-length body must not skip it.
                 * Do NOT add a `request.body != NULL` clause here — this
                 * branch is only reached while body is still NULL, so such a
                 * guard is never true and silently drops every POST body
                 * (CGI stdin empty, chat endpoint unable to parse). */
                if (have >= request.content_length &&
                    response.status_code == 0) {
                    if (request.content_length > 0) {
                        request.body = malloc(request.content_length + 1);
                        if (request.body) {
                            memcpy(request.body, buffer + body_start, request.content_length);
                            request.body[request.content_length] = '\0';
                        }
                    }
                    /* Everything from here on in buffer belongs to a
                     * subsequent pipelined request: remember its offset so
                     * the next keep-alive iteration can carry it forward
                     * instead of dropping it (HTTP/1.1 pipelining). */
                    prev_consumed = (long)header_end_len + request.content_length;
                    carry = total - (int)prev_consumed;
                }
            }

        /* Native C chat endpoint (src/llm.c): the LLM data path streams
         * SSE straight from this process (forked curl upstream), taking
         * priority over the /react FastCGI relay even when -R is set. */
        if (g_vite_upstream_port > 0 &&
            is_websocket_upgrade(buffer, (size_t)header_end_len)) {
            /* HMR WebSocket: splice to the internal Vite server. */
            ws_tunnel(client_fd, buffer, (size_t)header_end_len);
            return;
        }
        if (g_vite_upstream_port > 0 && is_vite_proxy_route(&request)) {
            proxy_to_vite(client_fd, buffer, (size_t)header_end_len, &request);
            return;
        }
        if (llm_is_chat_route(request.path, request.method)) {
            llm_handle_chat(&request, &response, client_fd);
        } else if (strncmp(request.path, "/react", 6) == 0 &&
            (request.path[6] == '\0' || request.path[6] == '/')) {
            if (g_react_sock[0]) {
                const char *ip = client_addr ? inet_ntoa(client_addr->sin_addr) : "-";
                int fcgi_body_bytes = 0;
                int status = forward_to_fcgi(g_react_sock, &request, ip, client_fd,
                                             &fcgi_body_bytes);
                if (status < 0) {
                    /* backend unreachable / protocol error before any byte was
                     * streamed: render our own 502 (forward_to_fcgi streamed
                     * nothing, so this page is the only response). */
                    set_error_response(&response, 502, "Bad Gateway");
                } else {
                    log_request(ip, &request, status, fcgi_body_bytes);
                    close(client_fd);
                    return;
                }
            } else {
                /* No SSR backend configured (slim image, no -R flag): the
                 * SSR-only chat page would be a bare 404. Degrade to the
                 * static zero-dependency chat UI; the chat API itself is
                 * handled natively by llm_handle_chat above. */
                if (strcmp(request.path, "/react/chat") == 0 ||
                    strcmp(request.path, "/react/") == 0 ||
                    strcmp(request.path, "/react") == 0) {
                    snprintf(request.path, sizeof(request.path), "/chat.html");
                }
                process_request(&request, &response, client_fd);
            }
        } else {
            process_request(&request, &response, client_fd);
        }
    }

    /* Streaming dispatchers (SSE chat) already wrote head + body to the
     * socket; log and close — no response serialization, no keep-alive
     * (SSE is close-delimited). */
    if (response.handled) {
        log_request(client_ip, &request, response.status_code, response.body_length);
        free(request.body);
        close(client_fd);
        return;
    }

    if (response.body == NULL && response.stream_path == NULL &&
        response.status_code != 0 && response.status_code != 304) {
        /* Body-less failure statuses get their error page here. 304 is
         * body-less by design (RFC 9110 15.4.5) and must not be replaced. */
        set_error_response(&response, response.status_code, response.status_text);
    }

    if (response.content_type[0] == '\0') {
        strcpy(response.content_type, "text/html");
    }

    int head_only = (strcmp(request.method, "HEAD") == 0);
    /* RFC 7230: HTTP/1.1 is persistent by default; HTTP/1.0 needs an
     * explicit "Connection: keep-alive". An explicit "close" always wins. */
    int keep_alive = (strncmp(request.protocol, "HTTP/1.1", 8) == 0);
    if (request.connection[0]) {
        if (strncasecmp(request.connection, "close", 5) == 0) {
            keep_alive = 0;
        } else if (strncasecmp(request.connection, "keep-alive", 10) == 0) {
            keep_alive = 1;
        }
    }
    if (keep_alive_force_close) {
        keep_alive = 0;
    }
    if (request.chunked) {
        /* Keep-alive pipelining after a chunked body is not decoded for
         * carry-over (the trailer's end offset is awkward to track), so
         * close instead of risking a swallowed request. Valid per HTTP/1.1. */
        keep_alive = 0;
    }
    int response_len;
    int body_sent = build_response(&response, head_only, keep_alive, response_buffer, &response_len);
    {
        int off = 0;
        while (off < response_len) {
            ssize_t sw = send(client_fd, response_buffer + off, (size_t)(response_len - off), 0);
            if (sw <= 0) {
                if (sw < 0 && errno == EINTR) continue;
                keep_alive = 0;
                break;
            }
            off += (int)sw;
        }
    }

    /* stream out-of-buffer bodies (large static files, 206 byte ranges).
     * Zero-copy sendfile(2) on Linux/macOS; fread/send fallback elsewhere.
     * The stream starts at stream_offset (0 = whole file). Note the loop
     * credits only actually-sent bytes, so partial sends no longer drop
     * data the way a fixed-chunk resend would. */
    if (!head_only && response.stream_path) {
        int sent_ok = 0;
#if defined(__APPLE__) || defined(__linux__)
        int sfd = open(response.stream_path, O_RDONLY);
        if (sfd >= 0) {
            off_t off = response.stream_offset;
            size_t remaining = (size_t)response.body_length;
            sent_ok = 1;
            while (remaining > 0) {
#if defined(__APPLE__)
                /* macOS argument order: sendfile(FILE fd, SOCKET fd, ...) */
                off_t chunk_len = (off_t)remaining;
                ssize_t w = sendfile(sfd, client_fd, off, &chunk_len, NULL, 0);
                if (w < 0) {
                    if (errno == EINTR || errno == EAGAIN) {
                        if (chunk_len > 0) { /* partial send: credit and retry */
                            off += chunk_len;
                            remaining -= (size_t)chunk_len;
                        }
                        continue;
                    }
                    sent_ok = 0; /* client gone or file trouble */
                    break;
                }
                off += chunk_len;
                remaining -= (size_t)chunk_len;
#else /* __linux__ */
                ssize_t w = sendfile(client_fd, sfd, &off, remaining);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    sent_ok = 0;
                    break;
                }
                if (w == 0) { /* premature EOF */
                    sent_ok = 0;
                    break;
                }
                remaining -= (size_t)w;
#endif
            }
            close(sfd);
        }
#else
        FILE *file = fopen(response.stream_path, "rb");
        if (file) {
            sent_ok = 1;
            if (response.stream_offset > 0 &&
                fseeko(file, response.stream_offset, SEEK_SET) != 0) {
                sent_ok = 0;
            }
            char chunk[65536];
            size_t remaining = (size_t)response.body_length;
            while (sent_ok && remaining > 0) {
                size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
                size_t n = fread(chunk, 1, want, file);
                if (n == 0) { sent_ok = 0; break; }
                size_t done = 0;
                while (done < n) {
                    ssize_t w = send(client_fd, chunk + done, n - done, 0);
                    if (w <= 0) {
                        if (w < 0 && errno == EINTR) continue;
                        sent_ok = 0;
                        break;
                    }
                    done += (size_t)w;
                }
                remaining -= done;
            }
            fclose(file);
        }
#endif
        if (sent_ok) {
            body_sent += response.body_length;
        } else {
            keep_alive = 0; /* headers promised a body we could not deliver */
        }
    }

    log_request(client_ip, &request, response.status_code, body_sent);

    free(response.body);
    if (response.stream_path) {
        if (response.stream_is_temp) unlink(response.stream_path);
        free(response.stream_path);
    }
    free(request.body);

    served++;
    if (!keep_alive ||
        served >= MAX_KEEPALIVE_REQUESTS ||
        response.status_code >= 500 ||
        g_shutdown_requested) {
        /* shutdown drain: finish this exchange, then close instead of
         * idling on keep-alive - the worker exits after this connection */
        close(client_fd);
        return;
    }
    } /* persistent-connection loop */
}
