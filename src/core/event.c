/* Master event loop for the fast path.
 *
 * Model: one non-blocking process accepts and serves everything that can
 * complete in-process — GET/HEAD static files, redirects, 304s, error
 * pages, /health — while requests that need the blocking pipeline (CGI,
 * the native chat SSE route, the /react FastCGI relay, anything with a
 * body) are handed to the prefork worker pool over SCM_RIGHTS.
 *
 * The request header is inspected with recv(MSG_PEEK): nothing is consumed
 * until the loop decides. A slow request therefore arrives at its worker
 * with the header bytes still in the kernel buffer — handle_client() reads
 * it exactly as if it had accepted the connection itself. kqueue(macOS) /
 * epoll(Linux) provide readiness notifications. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

#include "internal.h"
#include "metrics.h"
#include "llm.h"
#include "skills.h"
#include "tools.h"
#include "mcp.h"
#include "router.h"

#define EVENT_MAX_CONN 1024
#define EVENT_EV_BATCH 64
/* How long a fast-path response may wait for socket room before the client is
 * considered stalled (the loop socket is non-blocking: EAGAIN means "later"). */
#define FAST_SEND_WAIT_MS 5000

typedef struct {
    int fd;
    int eof;
} LoopEvent;

typedef enum {
    S_READ_HEADER,   /* peek the request header, then classify */
    S_SLOW_PENDING,  /* classified slow; waiting for a free worker */
} ConnState;

typedef struct {
    int fd;
    ConnState state;
    unsigned int ip;             /* network-order client addr */
    char ip_str[INET_ADDRSTRLEN];
    time_t deadline;             /* header/idle timeout */
    int served;                  /* requests served on this connection */
    int in_use;
} Conn;

static Conn g_conns[EVENT_MAX_CONN];
static Conn *g_slow_queue[EVENT_MAX_CONN];
static int g_slow_head = 0;
static int g_slow_tail = 0;
static int g_kq = -1;            /* kqueue() or epoll() fd */

#if defined(__linux__)
static void ev_register(int fd) {
    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = EPOLLIN;
    e.data.fd = fd;
    (void)epoll_ctl(g_kq, EPOLL_CTL_ADD, fd, &e);
}
static void ev_deregister(int fd) {
    (void)epoll_ctl(g_kq, EPOLL_CTL_DEL, fd, NULL);
}
#else
static void ev_register(int fd) {
    struct kevent ke;
    EV_SET(&ke, (uintptr_t)fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    (void)kevent(g_kq, &ke, 1, NULL, 0, NULL);
}
static void ev_deregister(int fd) {
    struct kevent ke;
    EV_SET(&ke, (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    (void)kevent(g_kq, &ke, 1, NULL, 0, NULL);
}
#endif

static const char *find_header_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

static Conn *conn_by_fd(int fd) {
    for (int i = 0; i < EVENT_MAX_CONN; i++) {
        if (g_conns[i].in_use && g_conns[i].fd == fd) return &g_conns[i];
    }
    return NULL;
}

static void conn_release(Conn *c) {
    c->in_use = 0;
    c->fd = -1;
}

static void conn_close(Conn *c) {
    ev_deregister(c->fd);
    close(c->fd);
    conn_release(c);
}

static void drain_slow_queue(void) {
    while (g_slow_head != g_slow_tail) {
        Conn *c = g_slow_queue[g_slow_head];
        char tok = 0;
        if (!pool_claim_slot_nb(&tok)) break; /* pool busy: wait for token event */
        if (pool_dispatch_fd(c->fd) < 0) {
            pool_return_token(tok);
            close(c->fd);
        } else {
            close(c->fd); /* master copy; the worker owns its own */
        }
        conn_release(c);
        g_slow_head = (g_slow_head + 1) % EVENT_MAX_CONN;
    }
}

/* Classify the just-peeked request as slow and park it on the queue. Any
 * response state built for the fast path is discarded (the worker rebuilds
 * it from the untouched socket buffer). */
static void go_slow(Conn *c) {
    METRICS_INC_AT(requests_total, 1); /* handed to the blocking pool */
    ev_deregister(c->fd);
    c->state = S_SLOW_PENDING;
    if ((g_slow_tail + 1) % EVENT_MAX_CONN == g_slow_head) {
        conn_close(c); /* queue full: drop rather than deadlock */
        return;
    }
    g_slow_queue[g_slow_tail] = c;
    g_slow_tail = (g_slow_tail + 1) % EVENT_MAX_CONN;
    drain_slow_queue();
}

static int want_keep_alive(const HttpRequest *req) {
    int ka = (strncmp(req->protocol, "HTTP/1.1", 8) == 0);
    if (req->connection[0]) {
        if (strncasecmp(req->connection, "close", 5) == 0) ka = 0;
        else if (strncasecmp(req->connection, "keep-alive", 10) == 0) ka = 1;
    }
    return ka;
}

static void recv_consume(int fd, int n) {
    char scratch[1024];
    while (n > 0) {
        int want = n < (int)sizeof(scratch) ? n : (int)sizeof(scratch);
        ssize_t r = recv(fd, scratch, (size_t)want, 0);
        if (r <= 0) break;
        n -= (int)r;
    }
}

/* Serve a fast request to completion: serialize, send, then either loop
 * back to header-read on keep-alive or close. Mirrors handle_client's
 * post-dispatch serialization (error-page fill, default content type,
 * keep-alive accounting). */
static void fast_serve(Conn *c, HttpRequest *req, HttpResponse *resp, size_t hdr_len, int force_close) {
    if (resp->body == NULL && resp->stream_path == NULL &&
        resp->status_code != 0 && resp->status_code != 304) {
        set_error_response(resp, resp->status_code, resp->status_text);
    }
    if (resp->content_type[0] == '\0') {
        strcpy(resp->content_type, "text/html");
    }

    int head_only = (strcmp(req->method, "HEAD") == 0);
    int keep_alive = want_keep_alive(req) && !force_close;
    char raw[MAX_RESPONSE_SIZE];
    int raw_len = 0;
    int body_sent = build_response(resp, head_only, keep_alive, raw, &raw_len);

    recv_consume(c->fd, (int)hdr_len); /* drop the peeked header */

    /* Non-blocking send (accept_http set O_NONBLOCK): a full send buffer is
     * "come back later", not a dead peer. Wait for room instead of abandoning
     * a response whose Content-Length already promised every byte. Bounded so
     * one stalled client cannot pin the loop forever - after that we drop the
     * connection exactly as the old code did, only now it takes a real stall
     * rather than the first full socket buffer. */
    int off = 0;
    while (off < raw_len) {
        ssize_t sw = send(c->fd, raw + off, (size_t)(raw_len - off), 0);
        if (sw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd pfd;
            pfd.fd = c->fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, FAST_SEND_WAIT_MS) <= 0) {
                keep_alive = 0;
                break;
            }
            continue;
        }
        if (sw <= 0) {
            keep_alive = 0;
            break;
        }
        off += (int)sw;
    }
    if (off < raw_len) {
        /* Headers went out promising a body the client never got. Log 0 body
         * bytes rather than the intended length, so a truncated response is
         * visible in the access log instead of looking like a clean 200. */
        body_sent = 0;
    }

    log_request(c->ip_str, req, resp->status_code, body_sent);
    METRICS_INC_AT(requests_total, 0); /* served on the fast path */
    free(resp->body);
    free(req->body);

    c->served++;
    if (!keep_alive || c->served >= MAX_KEEPALIVE_REQUESTS ||
        resp->status_code >= 500 || g_shutdown_requested) {
        conn_close(c);
        return;
    }
    c->state = S_READ_HEADER;
    c->deadline = time(NULL) + KEEPALIVE_TIMEOUT_SECONDS;
}

static void conn_readable(Conn *c) {
    char tmp[MAX_REQUEST_SIZE];
    ssize_t n = recv(c->fd, tmp, sizeof(tmp), MSG_PEEK | MSG_DONTWAIT);
    if (n <= 0) {
        conn_close(c); /* EOF / reset */
        return;
    }

    const char *hdr_end = find_header_end(tmp, (size_t)n);
    if (!hdr_end) {
        if (n >= (ssize_t)sizeof(tmp)) {
            /* RFC 9110 9.5.6: framing is unrecoverable. */
            HttpResponse r;
            memset(&r, 0, sizeof(r));
            set_error_response(&r, 431, "Request Header Fields Too Large");
            HttpRequest q;
            memset(&q, 0, sizeof(q));
            snprintf(q.remote_addr, sizeof(q.remote_addr), "%s", c->ip_str);
            strcpy(q.method, "GET");
            fast_serve(c, &q, &r, (size_t)n, 1);
        }
        return; /* wait for more bytes */
    }

    size_t hdr_len = (size_t)(hdr_end + 4 - tmp);
    if (time(NULL) >= c->deadline) {
        HttpRequest q;
        memset(&q, 0, sizeof(q));
        snprintf(q.remote_addr, sizeof(q.remote_addr), "%s", c->ip_str);
        strcpy(q.method, "GET");
        HttpResponse r;
        memset(&r, 0, sizeof(r));
        set_error_response(&r, 408, "Request Timeout");
        fast_serve(c, &q, &r, hdr_len, 1);
        return;
    }

    HttpRequest req;
    HttpResponse resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    snprintf(req.remote_addr, sizeof(req.remote_addr), "%s", c->ip_str);

    if (parse_request(tmp, &req) < 0) {
        set_error_response(&resp, 400, "Bad Request");
        fast_serve(c, &req, &resp, hdr_len, 0);
        return;
    }
    if (g_rate_limit_rps > 0 && !rate_limit_allow(rate_limit_client_ip(&req, c->ip))) {
        set_error_response(&resp, 429, "Too Many Requests");
        resp.retry_after = 1;
        fast_serve(c, &req, &resp, hdr_len, 1);
        return;
    }
    if (!check_basic_auth(req.authorization)) {
        set_error_response(&resp, 401, "Unauthorized");
        resp.auth_realm = g_auth_realm;
        fast_serve(c, &req, &resp, hdr_len, 1);
        return;
    }

    if (g_vite_upstream_port > 0 &&
        is_websocket_upgrade(tmp, (size_t)n)) {
        go_slow(c); /* HMR tunnel runs on a blocking worker */
        return;
    }
    if (!is_fast_request(&req)) {
        go_slow(c);
        return;
    }

    process_request(&req, &resp, c->fd);
    if (resp.stream_path) {
        /* Body too big for the in-buffer path: the worker streams it
         * (sendfile). Nothing has been consumed, so hand the connection
         * over and let handle_client redo the request. */
        free(resp.body);
        if (resp.stream_is_temp) unlink(resp.stream_path);
        free(resp.stream_path);
        go_slow(c);
        return;
    }
    fast_serve(c, &req, &resp, hdr_len, 0);
}

static void accept_http(int server_fd) {
    for (;;) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int fd = accept(server_fd, (struct sockaddr *)&ca, &cl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break; /* EAGAIN etc.: no more pending */
        }
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        Conn *c = NULL;
        for (int i = 0; i < EVENT_MAX_CONN; i++) {
            if (!g_conns[i].in_use) { c = &g_conns[i]; break; }
        }
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->state = S_READ_HEADER;
        c->ip = ca.sin_addr.s_addr;
        snprintf(c->ip_str, sizeof(c->ip_str), "%s", inet_ntoa(ca.sin_addr));
        c->served = 0;
        c->deadline = time(NULL) + g_request_timeout_seconds;
        c->in_use = 1;
        ev_register(fd);
    }
}

static void accept_fcgi(int server_fd, int fcgi_fd) {
    struct sockaddr_un client_sa;
    socklen_t client_len = sizeof(client_sa);
    int client_fd = accept(fcgi_fd, (struct sockaddr *)&client_sa, &client_len);
    if (client_fd < 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        close(fcgi_fd);
        close(server_fd); /* don't leak the listening socket into the handler */
        if (g_kq >= 0) close(g_kq);
        int tfd = pool_token_fd();
        if (tfd >= 0) close(tfd);
        fastcgi_handle_connection(client_fd);
        _exit(0);
    } else if (pid > 0) {
        close(client_fd);
    } else {
        close(client_fd);
    }
}

int event_loop(int server_fd, int fcgi_fd) {
#if defined(__linux__)
    g_kq = epoll_create1(0);
#else
    g_kq = kqueue();
#endif
    if (g_kq < 0) {
        perror("kqueue/epoll");
        return -1;
    }

    /* Listeners must be non-blocking so accept() returns EAGAIN instead of
     * parking the whole loop when accept_http drains the backlog. */
    {
        int fl = fcntl(server_fd, F_GETFL, 0);
        fcntl(server_fd, F_SETFL, fl | O_NONBLOCK);
        if (fcgi_fd >= 0) {
            fl = fcntl(fcgi_fd, F_GETFL, 0);
            fcntl(fcgi_fd, F_SETFL, fl | O_NONBLOCK);
        }
    }

    ev_register(server_fd);
    if (fcgi_fd >= 0) ev_register(fcgi_fd);
    /* The pool's token pipe must NOT be registered here, tempting as the
     * "wake me when a worker frees up" semantics are: it is a counting
     * semaphore pre-filled with one token per worker, so on an idle pool it
     * is *permanently* readable, and readiness is level-triggered — the
     * loop would spin at 100% CPU draining nothing (the handler only
     * consumes tokens when a slow connection is actually dispatched).
     * Slow-queue draining needs no notification anyway: go_slow() drains
     * synchronously when parking, and the loop top drains every iteration,
     * so dispatch latency after a token appears is bounded by the 100ms
     * epoll/kevent timeout below — imperceptible for CGI/SSE workloads. */

    while (g_server_running) {
        drain_slow_queue();

        if (g_reopen_log) {
            if (g_log_fp) fclose(g_log_fp);
            g_log_fp = fopen(g_log_path, "a");
            if (g_log_fp) fchmod(fileno(g_log_fp), 0600);
            g_reopen_log = 0;
        }
        if (g_resync) {
            g_resync = 0;
            printf("Catalog resync (SIGHUP): router sync + index rebuild...\n");
            fflush(stdout);
            router_sync_all();
            skills_init();
            mcp_init();
            printf("Catalog resync done: %d skill(s), %d tool(s), %d mcp server(s).\n",
                   skills_count(), tools_count(), mcp_server_count());
            fflush(stdout);
        }

        LoopEvent evs[EVENT_EV_BATCH];
        int n = 0;
#if defined(__linux__)
        struct epoll_event ep_evs[EVENT_EV_BATCH];
        int epn = epoll_wait(g_kq, ep_evs, EVENT_EV_BATCH, 100);
        if (epn < 0 && errno == EINTR) continue;
        for (int i = 0; i < epn && i < EVENT_EV_BATCH; i++) {
            evs[i].fd = ep_evs[i].data.fd;
            evs[i].eof = (ep_evs[i].events & (EPOLLHUP | EPOLLERR)) != 0;
        }
        n = epn;
#else
        struct timespec ts = {0, 100 * 1000 * 1000}; /* 100ms: deadline sweep */
        struct kevent ks[EVENT_EV_BATCH];
        n = kevent(g_kq, NULL, 0, ks, EVENT_EV_BATCH, &ts);
        if (n < 0 && errno == EINTR) continue;
        for (int i = 0; i < n; i++) {
            evs[i].fd = (int)ks[i].ident;
            evs[i].eof = (ks[i].flags & EV_EOF) != 0;
        }
#endif
        if (n < 0) continue;

        for (int i = 0; i < n; i++) {
            int fd = evs[i].fd;
            if (evs[i].eof) {
                Conn *c = conn_by_fd(fd);
                if (c) conn_close(c);
                continue;
            }
            if (fd == server_fd) {
                accept_http(server_fd);
            } else if (fcgi_fd >= 0 && fd == fcgi_fd) {
                accept_fcgi(server_fd, fcgi_fd);
            } else {
                Conn *c = conn_by_fd(fd);
                if (c && c->state == S_READ_HEADER) conn_readable(c);
            }
        }

        /* idle deadline sweep */
        time_t now = time(NULL);
        for (int i = 0; i < EVENT_MAX_CONN; i++) {
            if (g_conns[i].in_use && g_conns[i].deadline &&
                now >= g_conns[i].deadline) {
                conn_close(&g_conns[i]);
            }
        }
    }

    close(g_kq);
    g_kq = -1;
    return 0;
}
