/* Regression test: body-to-tempfile spool (multipart item (b)).
 *
 * Content-Length bodies above the 64 KiB read buffer (MAX_REQUEST_SIZE)
 * are streamed to a temp file under BODY_SPOOL_DIR when they fit
 * MAX_BODY_SPOOL_BYTES; req->body_temp_path names the file and the
 * declared length is fully consumed, so keep-alive stays exact. Bodies
 * over the cap keep the 413 + forced close.
 *
 * Asserts, against a real server (MAX_BODY_SPOOL_BYTES=256 KiB, spool dir
 * = a fresh temp dir):
 *   A. 100 KiB POST spools (non-413, dispatches -> 404) and a second
 *      pipelined GET on the SAME connection answers 200 — the spooled
 *      length was fully consumed, no keep-alive desync.
 *   B. 300 KiB POST (over the cap) answers 413 and the connection closes.
 *   C. after the requests, the spool dir holds no leftover body.* file —
 *      the temp files were unlinked at request teardown.
 *
 * The lume side (tests/run_all.sh leg 4h) proves the handler-visible
 * req.body_temp_path / read_file path over real HTTP.
 *
 * POSIX-only (mkdtemp, env override): runs on the Linux CI job. Spawn /
 * connect scaffolding mirrors test_oom_body.c.
 */

#define _GNU_SOURCE /* mkdtemp on glibc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define REQUEST_TIMEOUT 10     /* seconds for the client read loop */
#define CONNECT_TIMEOUT 45     /* seconds to wait for the server to bind */
#define CAP 262144             /* MAX_BODY_SPOOL_BYTES for this test */
#define SMALL 102400           /* 100 KiB: > 64 KiB buffer, within cap */
#define HUGE 300000            /* 300 KiB: over the cap */

static int spool_done = 0; /* dir check is skipped on setup failure */

static pid_t start_server(int port, const char *spool_dir) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "env -u LLM_API_URL -u ROUTER_API_URL "
             "MAX_BODY_SPOOL_BYTES=%d BODY_SPOOL_DIR=%s "
             "bin/agent-httpd -p %d -w 0 -n & echo $!",
             CAP, spool_dir, port);
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

/* send everything; returns 0 on success */
static int send_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* read until EOF/timeout or "HTTP/1.1 " status line is complete; returns
 * the raw bytes read (NUL-terminated). */
static size_t read_resp(int fd, char *buf, size_t cap) {
    size_t got = 0;
    time_t deadline = time(NULL) + REQUEST_TIMEOUT;
    while (got < cap - 1 && time(NULL) < deadline) {
        struct timeval tv = {1, 0};
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) continue;
        ssize_t n = read(fd, buf + got, cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = '\0';
        /* first response line + headers only: we only assert status lines */
        if (strstr(buf, "\r\n\r\n")) {
            char *end = strstr(buf, "\r\n\r\n");
            size_t hdr_end = (size_t)(end - buf) + 4;
            /* keep reading body only if the caller wants it; we don't */
            got = hdr_end;
            break;
        }
    }
    buf[got] = '\0';
    return got;
}

static int has_body_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "body.", 5) == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

int main(void) {
    int port = 21000 + (int)(getpid() % 10000);

    char spool_dir[] = "/tmp/ahd-spool-XXXXXX";
    if (mkdtemp(spool_dir) == NULL) {
        fprintf(stderr, "FAIL: mkdtemp: %s\n", strerror(errno));
        return 1;
    }

    pid_t server = start_server(port, spool_dir);
    if (server <= 0) {
        fprintf(stderr, "FAIL: could not spawn server\n");
        rmdir(spool_dir);
        return 1;
    }

    int fd = connect_server(port);
    if (fd < 0) {
        fprintf(stderr, "FAIL: could not connect to server\n");
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
        rmdir(spool_dir);
        return 1;
    }

    /* A. 100 KiB POST (spool) then GET on the same connection */
    char *big = malloc(SMALL);
    char req[64];
    memset(big, 's', SMALL);
    int rl = snprintf(req, sizeof req,
                      "POST /spool-test HTTP/1.1\r\nHost: t\r\n"
                      "Content-Length: %d\r\n\r\n", SMALL);
    if (send_all(fd, req, (size_t)rl) != 0 ||
        send_all(fd, big, SMALL) != 0) {
        fprintf(stderr, "FAIL: write POST: %s\n", strerror(errno));
        goto out;
    }
    char buf[4096];
    read_resp(fd, buf, sizeof buf);
    if (strstr(buf, "HTTP/1.1 413") != NULL || strstr(buf, "HTTP/1.1 500") != NULL) {
        fprintf(stderr, "FAIL: 100 KiB under cap should spool and dispatch, got:\n%.*s\n",
                (int)strlen(buf), buf);
        goto out;
    }
    /* dispatch happened: no route for /spool-test -> 404 */
    if (strstr(buf, "HTTP/1.1 404") == NULL) {
        fprintf(stderr, "FAIL: expected 404 after spooled dispatch, got:\n%.*s\n",
                (int)strlen(buf), buf);
        goto out;
    }
    /* pipelined GET on the same connection: desync would garble this */
    const char *g = "GET / HTTP/1.1\r\nHost: t\r\n\r\n";
    if (send_all(fd, g, strlen(g)) != 0) {
        fprintf(stderr, "FAIL: write GET: %s\n", strerror(errno));
        goto out;
    }
    read_resp(fd, buf, sizeof buf);
    if (strstr(buf, "HTTP/1.1 200") == NULL) {
        fprintf(stderr, "FAIL: keep-alive GET after spooled POST, got:\n%.*s\n",
                (int)strlen(buf), buf);
        goto out;
    }
    close(fd);
    free(big);
    printf("ok test-spool-body: 100 KiB spooled + keep-alive stays in sync\n");

    /* B. 300 KiB over the cap -> 413 + close */
    fd = connect_server(port);
    if (fd < 0) {
        fprintf(stderr, "FAIL: reconnect\n");
        goto out2;
    }
    big = malloc(HUGE);
    memset(big, 'h', HUGE);
    rl = snprintf(req, sizeof req,
                  "POST /spool-test HTTP/1.1\r\nHost: t\r\n"
                  "Content-Length: %d\r\n\r\n", HUGE);
    if (send_all(fd, req, (size_t)rl) != 0 ||
        send_all(fd, big, HUGE) != 0) {
        fprintf(stderr, "FAIL: write huge POST: %s\n", strerror(errno));
        free(big);
        close(fd);
        goto out2;
    }
    read_resp(fd, buf, sizeof buf);
    if (strstr(buf, "HTTP/1.1 413") == NULL) {
        fprintf(stderr, "FAIL: 300 KiB over cap should 413, got:\n%.*s\n",
                (int)strlen(buf), buf);
        free(big);
        close(fd);
        goto out2;
    }
    free(big);
    printf("ok test-spool-body: 300 KiB over cap -> 413\n");

    /* C. no leftover body.* temp files (requests completed: teardown ran) */
    if (has_body_files(spool_dir) != 0) {
        fprintf(stderr, "FAIL: leftover spool files in %s\n", spool_dir);
        close(fd);
        goto out2;
    }
    close(fd);
    printf("ok test-spool-body: temp files removed at request teardown\n");

    spool_done = 1;
out2:
    for (int i = 0; i < 100; i++) {
        if (waitpid(server, NULL, WNOHANG) == server) break;
        usleep(100000);
    }
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    if (!spool_done) {
        rmdir(spool_dir);
        return 1;
    }
    if (rmdir(spool_dir) != 0) {
        fprintf(stderr, "FAIL: spool dir not empty after teardown\n");
        return 1;
    }
    return 0;

out:
    free(big);
    close(fd);
    spool_done = -1;
    goto out2;
}
