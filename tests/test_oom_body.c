/* Regression test: request-body allocation failure must answer 500 and
 * force-close, never dispatch with body == NULL (which would dereference
 * it in form_field() and crash the worker).
 *
 * Mechanism: LD_PRELOAD tests/fault_malloc.so + AH_OOM_BYTES=<body+1>
 * makes exactly the body buffer's malloc() fail inside the real server.
 * The fixed path replies HTTP/1.1 500 (error page, then close); the
 * unfixed path either crashes the worker (connection drops with no HTTP
 * response) or dispatches a broken request.
 *
 * POSIX-only (LD_PRELOAD, fork-per-connection spawn): runs on the Linux
 * CI job, not on mingw. Spawn/connect scaffolding mirrors
 * test_keepalive_pipeline.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define REQUEST_TIMEOUT 10     /* seconds for the client read loop */
#define CONNECT_TIMEOUT 45     /* seconds to wait for the server to bind */
#define BODY_SIZE 4096         /* bytes; injected failure targets BODY_SIZE+1 */
#define SO_PATH "tests/fault_malloc.so"

static pid_t start_server(int port) {
    /* Same isolation as the keep-alive test: drop LLM/router env so
     * startup does not stall on a 10s router-sync timeout. */
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "env -u LLM_API_URL -u ROUTER_API_URL "
             "LD_PRELOAD=%s AH_OOM_BYTES=%d "
             "bin/agent-httpd -p %d -w 0 -n & echo $!",
             SO_PATH, BODY_SIZE + 1, port);
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
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    for (int i = 0; i < CONNECT_TIMEOUT * 20; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0)
            return fd;
        close(fd);
        usleep(50000);
    }
    return -1;
}

int main(void) {
    int port = 20000 + (int)(getpid() % 10000);
    pid_t server = start_server(port);
    if (server <= 0) {
        fprintf(stderr, "FAIL: could not spawn server\n");
        return 1;
    }
    int fd = connect_server(port);
    if (fd < 0) {
        fprintf(stderr, "FAIL: could not connect to server\n");
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
        return 1;
    }

    char body[BODY_SIZE];
    memset(body, 'x', sizeof body);
    char req[64];
    int rl = snprintf(req, sizeof req,
                      "POST /login HTTP/1.1\r\nHost: t\r\nContent-Length: %d\r\n\r\n",
                      BODY_SIZE);
    if (write(fd, req, (size_t)rl) != rl ||
        write(fd, body, sizeof body) != (ssize_t)sizeof body) {
        fprintf(stderr, "FAIL: write request: %s\n", strerror(errno));
        close(fd);
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
        return 1;
    }

    /* Read the response; the fixed path sends "HTTP/1.1 500 ..." and
     * closes. A worker crash drops the connection with no status line. */
    char buf[512];
    size_t got = 0;
    time_t deadline = time(NULL) + REQUEST_TIMEOUT;
    while (got < sizeof buf - 1 && time(NULL) < deadline) {
        struct timeval tv = {1, 0};
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) continue;
        ssize_t n = read(fd, buf + got, sizeof buf - 1 - got);
        if (n == 0) break; /* server closed */
        if (n < 0) break;
        got += (size_t)n;
        if (strstr(buf, " 500 ")) break;
    }
    buf[got] = '\0';

    close(fd);
    int ok = strstr(buf, "HTTP/1.1 500") != NULL;
    /* wait for the server to exit so a forked worker cannot linger */
    for (int i = 0; i < 100; i++) {
        if (waitpid(server, NULL, WNOHANG) == server) break;
        usleep(100000);
    }
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);

    if (!ok) {
        fprintf(stderr, "FAIL: expected HTTP/1.1 500 for body OOM, got:\n%.*s\n",
                (int)got, buf);
        return 1;
    }
    printf("ok test-oom-body: body malloc failure -> 500 + close (LD_PRELOAD fault_malloc)\n");
    return 0;
}
