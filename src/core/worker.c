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
static pid_t *worker_pids = NULL;
static int nworkers_running = 0;

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
static void worker_loop(int dispatch_sock, int my_token_w) {
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
        int fd = recv_fd_from_worker(dispatch_sock);
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
            close(dp[0]);   /* keep dp[1] + tp[1] */
            close(tp[0]);
            worker_loop(dp[1], tp[1]); /* never returns */
        }
        worker_pids[started++] = pid;
    }
    close(dp[1]); /* parent keeps dp[0] (dispatch) + both token ends */
    dispatch_fd = dp[0];
    token_r = tp[0];
    token_w = tp[1];
    nworkers_running = started;
    /* The event loop drains the slow queue on token readability; it must
     * never block inside pool_claim_slot, so make the read end O_NONBLOCK. */
    (void)fcntl(token_r, F_SETFL, O_NONBLOCK);

    if (started == 0) {
        close(dispatch_fd);
        close(token_r);
        close(token_w);
        dispatch_fd = token_r = token_w = -1;
        free(worker_pids);
        worker_pids = NULL;
        return -1;
    }

    printf("Prefork worker pool: %d workers\n", started);
    return 0;
}

/* Block until a worker frees up (read 1 token); returns the token byte via
 * *tok_out, or 0 when the server is shutting down mid-wait. The token must
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

/* Non-blocking slot claim for the event loop: returns 1 with *tok_out set
 * when a worker is free, 0 when the pool is busy (or shutting down). Never
 * blocks; the loop wakes on token_fd readability to drain its queue. */
int pool_claim_slot_nb(char *tok_out) {
    char tok = 0;
    ssize_t r = read(token_r, &tok, 1);
    if (r == 1) {
        *tok_out = tok;
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
        free(worker_pids);
        worker_pids = NULL;
        close(token_r);
        close(token_w);
        close(dispatch_fd);
        token_r = token_w = dispatch_fd = -1;
    }
}
