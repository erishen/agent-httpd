/* --- prefork worker pool -------------------------------------------------
 *
 * Classic fork-per-connection pays fork() + COW page setup per request.
 * The pool pre-spawns N workers that each block on a UNIX socketpair; the
 * parent accept()s a connection and hands the fd over via SCM_RIGHTS.
 * Workers never touch the listening socket.
 *
 * Concurrency cap = a pipe used as a counting semaphore: it starts with N
 * token bytes (N = free workers). The parent reads 1 token before handing
 * off a connection; the worker writes its token back when done. This is
 * portable everywhere (unnamed POSIX sems are unimplemented on macOS),
 * needs no shm, and single-byte pipe ops are atomic.
 *
 * macOS marks unnamed sem_* APIs deprecated (in favor of named sem_open);
 * the unnamed form is not used here at all - see the token pipe above. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <poll.h>

#include "internal.h"
#include "metrics.h"

/* Set by the worker's SIGTERM handler: finish the current connection, then
 * exit (graceful drain). handle_client checks the same flag and stops the
 * keep-alive loop, so idle connections close instead of lingering. */
volatile sig_atomic_t g_shutdown_requested = 0;

static void worker_term_handler(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

static int token_r = -1;      /* semaphore pipe: parent reads (wait) */
static int token_w = -1;      /* workers write (post) */
static int dispatch_fd = -1;  /* parent->worker fd-passing socketpair */
static int dispatch_peer = -1;/* parent's copy of the worker end, for late forks */
static pid_t *worker_pids = NULL;
static int nworkers_running = 0;

/* --- elastic burst workers ------------------------------------------------
 *
 * A prefork slot is held for as long as a handler runs, and some handlers
 * legitimately run for minutes (LLM calls, AI diagnose). With a fixed pool,
 * eight such requests take every slot and the rest of the API layer 503s
 * until they finish. The pool therefore grows on demand: when the master has
 * a queued connection and no free token, it forks one extra worker up to
 * `burst_max`. Extra workers retire after `burst_idle` seconds of no work,
 * giving the capacity back to the token pipe.
 *
 * Invariant: token bytes == free workers. A spawning worker adds one token;
 * a retiring worker takes one back (tokens are fungible capacity units, so
 * consuming any single byte is exactly "remove one worker"). If the take
 * fails the master has already claimed a token for a dispatch aimed at this
 * worker, and it must serve that connection instead of retiring. */
static pid_t *burst_pids = NULL;
static int burst_max = 0;        /* extra workers allowed beyond the base pool */
static int nburst_running = 0;
static int burst_idle_seconds = 30;

static void worker_loop(int dispatch_sock, int my_token_w, int my_token_r,
                        int burst);

/* Send one connection fd to a worker over the socketpair. */
static int send_fd_to_worker(int sock, int fd) {
    struct msghdr msg;
    struct iovec iov;
    char buf[1] = {0};
    char cmsg_buf[CMSG_SPACE(sizeof(int))];

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}

/* Receive one connection fd (blocking; parent guarantees a free worker). */
static int recv_fd_from_worker(int sock) {
    struct msghdr msg;
    struct iovec iov;
    char buf[1];
    char cmsg_buf[CMSG_SPACE(sizeof(int))];

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    if (recvmsg(sock, &msg, 0) < 1) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

/* Worker: serve connections until told to stop. On SIGTERM the handler
 * only sets the flag - the worker finishes its current connection (the
 * keep-alive loop in handle_client stops accepting further requests)
 * and exits after releasing its slot. The parent drains with a deadline
 * and force-kills stragglers (pool_shutdown). */
static void worker_loop(int dispatch_sock, int my_token_w, int my_token_r,
                        int burst) {
    /* sigaction WITHOUT SA_RESTART: plain signal() would auto-restart the
     * blocking recvmsg below and the worker could never observe shutdown. */
    struct sigaction wsa;
    memset(&wsa, 0, sizeof(wsa));
    wsa.sa_handler = worker_term_handler;
    wsa.sa_flags = 0;
    sigemptyset(&wsa.sa_mask);
    sigaction(SIGTERM, &wsa, NULL);
    sigaction(SIGINT, &wsa, NULL);

    for (;;) {
        int fd;
        if (!burst) {
            fd = recv_fd_from_worker(dispatch_sock);
        } else {
            fd = -1;
            for (;;) {
                struct pollfd pf;
                pf.fd = dispatch_sock;
                pf.events = POLLIN;
                pf.revents = 0;
                int pr = poll(&pf, 1, burst_idle_seconds * 1000);
                if (pr < 0 && errno == EINTR) {
                    if (g_shutdown_requested) _exit(0);
                    continue;
                }
                if (pr > 0) {
                    fd = recv_fd_from_worker(dispatch_sock);
                    break;
                }
                /* Idle past the timeout: try to hand the capacity back. */
                char tok;
                if (read(my_token_r, &tok, 1) == 1) {
                    if (g_metrics) {
                        __atomic_fetch_sub(&g_metrics->workers_total, 1ULL,
                                           __ATOMIC_RELAXED);
                    }
                    _exit(0);
                }
                /* EAGAIN means the master already claimed our token and a
                 * connection is on its way: serve it, then try again. */
                pf.revents = 0;
                if (poll(&pf, 1, 1000) > 0) {
                    fd = recv_fd_from_worker(dispatch_sock);
                    break;
                }
                if (g_shutdown_requested) _exit(0);
            }
        }
        if (fd < 0) {
            /* shutdown signal interrupted the blocking wait (EINTR), or the
             * parent closed the dispatch socket: drain-time exit */
            _exit(0);
        }

        struct sockaddr_storage peer;
        socklen_t plen = sizeof(peer);
        const struct sockaddr *paddr = NULL;
        if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0) {
            paddr = (const struct sockaddr *)&peer;
        }
        handle_client(fd, paddr, plen); /* closes fd; returns after close */

        /* release the slot back to the pool */
        char tok = 1;
        ssize_t w = write(my_token_w, &tok, 1);
        (void)w;
        /* The master increments workers_busy when it claims a token; without
         * the matching decrement here the gauge only ever climbs and stops
         * meaning "in flight". Write the token first: a claim racing this can
         * then see +1 briefly, but never a negative that would wrap. */
        if (g_metrics) __atomic_fetch_sub(&g_metrics->workers_busy, 1ULL,
                                          __ATOMIC_RELAXED);

        if (g_shutdown_requested) _exit(0); /* drained: no further dispatch */
    }
}

int start_worker_pool(int n, int server_fd) {
    (void)server_fd;
    if (n < 1) n = 1;
    if (n > FDPASS_MAX_N) n = FDPASS_MAX_N;

    /* the counting-semaphore pipe, pre-filled with n tokens */
    int tp[2];
    if (pipe(tp) < 0) {
        perror("pipe");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        char tok = 1;
        if (write(tp[1], &tok, 1) != 1) {
            perror("token init");
            close(tp[0]);
            close(tp[1]);
            return -1;
        }
    }

    int dp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, dp) < 0) {
        perror("socketpair");
        close(tp[0]);
        close(tp[1]);
        return -1;
    }

    worker_pids = calloc((size_t)n, sizeof(pid_t));
    if (!worker_pids) {
        close(tp[0]);
        close(tp[1]);
        close(dp[0]);
        close(dp[1]);
        return -1;
    }

    int started = 0;
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork worker");
            break;
        }
        if (pid == 0) {
            /* keep dp[1] (worker end) and both pipe ends: the read end lets a
             * burst worker hand its capacity back when it retires. */
            close(dp[0]);
            worker_loop(dp[1], tp[1], tp[0], 0); /* never returns */
        }
        worker_pids[started++] = pid;
    }
    /* dp[1] stays open in the parent so late (burst) forks can hand it to a
     * child that inherits the same worker end. */
    dispatch_fd = dp[0];
    dispatch_peer = dp[1];
    token_r = tp[0];
    token_w = tp[1];
    nworkers_running = started;
    /* The event loop drains the slow queue on token readability; it must
     * never block inside pool_claim_slot, so make the read end O_NONBLOCK. */
    (void)fcntl(token_r, F_SETFL, O_NONBLOCK);

    /* Elastic headroom: WORKERS_MAX caps the pool, default double the base.
     * Extra workers are only forked when every slot is busy and connections
     * are waiting, so a long AI/LLM job cannot starve the rest of the API. */
    int max_total = started * 2;
    const char *env_max = getenv("WORKERS_MAX");
    if (env_max && *env_max) {
        int v = atoi(env_max);
        if (v > 0) max_total = v;
    }
    if (max_total > FDPASS_MAX_N) max_total = FDPASS_MAX_N;
    burst_max = max_total > started ? max_total - started : 0;
    const char *env_idle = getenv("POOL_BURST_IDLE_SECONDS");
    if (env_idle && *env_idle) {
        int v = atoi(env_idle);
        if (v > 0) burst_idle_seconds = v;
    }
    if (burst_max > 0) {
        burst_pids = calloc((size_t)burst_max, sizeof(pid_t));
        if (!burst_pids) burst_max = 0; /* headroom optional: never fail startup */
    }
    if (g_metrics) g_metrics->workers_total = (unsigned long long)started;

    if (started == 0) {
        close(dispatch_fd);
        close(dispatch_peer);
        close(token_r);
        close(token_w);
        dispatch_fd = dispatch_peer = token_r = token_w = -1;
        free(worker_pids);
        worker_pids = NULL;
        return -1;
    }

    printf("Prefork worker pool: %d workers (max %d under load)\n",
           started, started + burst_max);
    return 0;
}

/* Block until a worker frees up (read 1 token); returns the token byte via * *tok_out, or 0 when the server is shutting down mid-wait. The token must
 * be returned via pool_return_token() if dispatch then fails. */
int pool_claim_slot(char *tok_out) {
    for (;;) {
        char tok = 0;
        ssize_t r = read(token_r, &tok, 1);
        if (r == 1) {
            *tok_out = tok;
            if (g_metrics) __atomic_fetch_add(&g_metrics->workers_busy,
                                             1ULL, __ATOMIC_RELAXED);
            return 1;
        }
        if (r == 0) return 0; /* write end closed: the pool is gone */
        if (errno == EINTR) {
            if (!g_server_running) return 0;
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!g_server_running) return 0;
            /* token_r is O_NONBLOCK (set by start_worker_pool so the event
             * loop can poll it without blocking). A bare read() therefore
             * returns EAGAIN the instant the pool is drained — which the
             * caller would misread as "shutting down" and answer by closing
             * the connection. Wait for a token explicitly instead; the 1s
             * tick keeps the shutdown flag observable. */
            struct pollfd pf;
            pf.fd = token_r;
            pf.events = POLLIN;
            pf.revents = 0;
            if (poll(&pf, 1, 1000) < 0 && errno != EINTR) return 0;
            continue;
        }
        return 0; /* unexpected errno: treat the pool as unusable */
    }
}

void pool_return_token(char tok) {
    ssize_t w = write(token_w, &tok, 1);
    if (w == 1 && g_metrics) {
        __atomic_fetch_sub(&g_metrics->workers_busy, 1ULL, __ATOMIC_RELAXED);
    }
}

int pool_token_fd(void) {
    return token_r;
}

/* Fork one extra worker because connections are waiting and no token is free.
 * Called from the event loop right after a failed claim, so the caller may
 * retry the claim immediately: the new token is written before this returns.
 * Returns 1 when a worker was spawned. */
int pool_maybe_grow(int queued) {
    if (queued <= 0 || burst_max <= 0 || nburst_running >= burst_max) return 0;
    if (!g_server_running) return 0;
    if (dispatch_peer < 0 || token_w < 0 || token_r < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        /* Keep the worker end + pipe ends; drop the master's dispatch end so
         * the child cannot accidentally accept dispatches itself. */
        close(dispatch_fd);
        worker_loop(dispatch_peer, token_w, token_r, 1); /* never returns */
    }

    char tok = 1;
    if (write(token_w, &tok, 1) != 1) {
        /* Without a token the extra worker would sit forever unclaimed. */
        kill(pid, SIGTERM);
        return 0;
    }
    burst_pids[nburst_running++] = pid;
    if (g_metrics) {
        __atomic_fetch_add(&g_metrics->workers_total, 1ULL, __ATOMIC_RELAXED);
    }
    return 1;
}

/* Non-blocking slot claim for the event loop: returns 1 with *tok_out set
 * when a worker is free, 0 when the pool is busy (or shutting down). Never
 * blocks; the loop wakes on token_fd readability to drain its queue. */
int pool_claim_slot_nb(char *tok_out) {
    char tok = 0;
    ssize_t r = read(token_r, &tok, 1);
    if (r == 1) {
        *tok_out = tok;
        /* Mirror pool_claim_slot(): every claim is one in-flight request, and
         * the worker decrements when it hands the token back. The counter
         * only stays honest if both claim paths bump it. */
        if (g_metrics) __atomic_fetch_add(&g_metrics->workers_busy, 1ULL,
                                          __ATOMIC_RELAXED);
        return 1;
    }
    return 0;
}

int pool_dispatch_fd(int client_fd) {
    return send_fd_to_worker(dispatch_fd, client_fd);
}

/* Graceful shutdown: SIGTERM every worker, then drain - give them a
 * deadline to finish in-flight requests (the keep-alive loop stops on the
 * shutdown flag). SIGCHLD is SIG_IGN in the parent, so exiting workers are
 * reaped automatically and kill(pid, 0) is a reliable liveness probe;
 * stragglers past the deadline are SIGKILLed. */
#define POOL_DRAIN_SECONDS 5

void pool_shutdown(void) {
    if (worker_pids && nworkers_running > 0) {
        for (int i = 0; i < nworkers_running; i++) {
            kill(worker_pids[i], SIGTERM);
        }
        for (int i = 0; i < nburst_running; i++) {
            kill(burst_pids[i], SIGTERM);
        }
        time_t deadline = time(NULL) + POOL_DRAIN_SECONDS;
        for (;;) {
            int alive = 0;
            for (int i = 0; i < nworkers_running; i++) {
                if (kill(worker_pids[i], 0) == 0) alive++;
            }
            if (alive == 0 || time(NULL) >= deadline) break;
            usleep(100 * 1000);
        }
        for (int i = 0; i < nworkers_running; i++) {
            kill(worker_pids[i], SIGKILL); /* no-op for exited workers */
        }
        for (int i = 0; i < nburst_running; i++) {
            kill(burst_pids[i], SIGKILL);
        }
        free(worker_pids);
        worker_pids = NULL;
        free(burst_pids);
        burst_pids = NULL;
        nburst_running = 0;
        burst_max = 0;
        close(token_r);
        close(token_w);
        close(dispatch_fd);
        close(dispatch_peer);
        token_r = token_w = dispatch_fd = dispatch_peer = -1;
    }
}
