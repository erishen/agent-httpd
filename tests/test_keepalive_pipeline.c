/* Regression test for fix D (keep-alive request pipelining).
 *
 * handle_client() in src/http.c keeps a persistent read buffer across
 * keep-alive iterations. If a client pipelines two requests into one TCP
 * segment, the old loop reset the buffer every iteration and DISCARDED the
 * second request that had already been recv()'d -- the client then hung
 * until timeout.
 *
 * This test starts the real server (fork-per-connection mode), opens one
 * connection, writes two GET requests in a single send(), and asserts that
 * TWO HTTP responses arrive. A 2s request timeout makes the unfixed path
 * fail fast (only one response, then the worker's recv times out and the
 * connection closes).
 *
 * The port is chosen per-run from the PID so repeated invocations never
 * collide with a leftover server from a previous run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define REQUEST_TIMEOUT 5 /* seconds for the client read loop */
#define CONNECT_TIMEOUT 45 /* seconds to wait for the server to bind */
#define SERVER_TIMEOUT "2" /* REQUEST_TIMEOUT_SECONDS for the spawned server */

static pid_t start_server(int port) {
    /* Short request timeout so the unfixed path fails fast instead of
     * hanging for the default 60s. */
    setenv("REQUEST_TIMEOUT_SECONDS", SERVER_TIMEOUT, 1);
    /* The keep-alive test does not need a live LLM/router; dropping
     * LLM_API_URL avoids a 10s router-sync timeout when one is set but
     * unreachable, which would otherwise delay startup past the connect
     * window. */
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "env -u LLM_API_URL -u ROUTER_API_URL bin/agent-httpd -p %d -w 0 -n & echo $!",
             port);
    FILE *p = popen(cmd, "r");
    if (!p) {
        perror("popen");
        return -1;
    }
    char pidbuf[32];
    if (!fgets(pidbuf, sizeof pidbuf, p)) {
        pclose(p);
        return -1;
    }
    pclose(p);
    return (pid_t)strtol(pidbuf, NULL, 10);
}

static int connect_server(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    /* The server spawns resident MCP subprocesses at startup, so binding
     * can take a couple of seconds; wait up to CONNECT_TIMEOUT. */
    for (int i = 0; i < CONNECT_TIMEOUT * 20; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) return fd;
        usleep(50000);
    }
    close(fd);
    return -1;
}

static int count_responses(const char *buf, size_t len) {
    int n = 0;
    const char *p = buf;
    const char *end = buf + len;
    while ((p = memmem(p, (size_t)(end - p), "HTTP/1.1 ", 9)) != NULL) {
        n++;
        p += 9;
    }
    return n;
}

int main(void) {
    int port = 19000 + (getpid() % 4000);

    pid_t srv = start_server(port);
    if (srv < 0) {
        fprintf(stderr, "FAIL: could not start server\n");
        return 1;
    }
    usleep(300000);

    int fd = connect_server(port);
    if (fd < 0) {
        fprintf(stderr, "FAIL: could not connect to server on port %d\n", port);
        kill(srv, SIGTERM);
        waitpid(srv, NULL, 0);
        return 1;
    }

    /* Two pipelined requests for a non-existent static path. A 404 is
     * served by the static handler with no backend/proxy involved, so it is
     * fast and deterministic regardless of dev-proxy / React configuration.
     * The point of the test is keep-alive pipelining: both requests must be
     * served, not just the first. */
    const char *req =
        "GET /__keepalive_pipeline_test__ HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /__keepalive_pipeline_test__ HTTP/1.1\r\nHost: localhost\r\n\r\n";
    if (send(fd, req, (size_t)strlen(req), 0) < 0) {
        perror("send");
        close(fd);
        kill(srv, SIGTERM);
        waitpid(srv, NULL, 0);
        return 1;
    }

    char buf[65536];
    size_t total = 0;
    int got = 0;
    fd_set rfds;
    struct timeval tv;
    while (total < sizeof buf - 1) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = REQUEST_TIMEOUT;
        tv.tv_usec = 0;
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) break; /* timeout or error: stop reading */
        ssize_t n = recv(fd, buf + total, sizeof buf - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        got = count_responses(buf, total);
        if (got >= 2) break; /* enough to prove pipelining works */
    }
    buf[total] = '\0';
    close(fd);

    int ok = (got >= 2);
    if (ok) {
        printf("keep-alive pipeline: got %d responses (>=2), pipelining OK\n", got);
    } else {
        printf("keep-alive pipeline: got %d responses (expected 2) -- "
               "second pipelined request was swallowed\n", got);
    }

    kill(srv, SIGTERM);
    waitpid(srv, NULL, 0);

    if (!ok) {
        fprintf(stderr, "FAIL: pipelined second request not served\n");
        return 1;
    }
    printf("ALL keep-alive pipeline TESTS PASSED\n");
    return 0;
}
