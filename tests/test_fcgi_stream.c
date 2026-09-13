/* End-to-end test for forward_to_fcgi (the streaming fix). A real FastCGI
 * backend over a UNIX socket returns a >64KB response; the relay must stream
 * it whole to the client fd instead of 502-ing on the old 64KB cap.
 *
 * The client fd is one end of a socketpair; a reader thread drains it
 * concurrently so forward_to_fcgi's send() never blocks on a full socket
 * buffer. Compiled standalone from src/cgi/fastcgi.c (no link against main.o);
 * -dead_strip (macOS) / -gc-sections (Linux) discards the unused
 * fastcgi_handle_connection so no project globals need stubbing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include "httpd.h"

#define FCGI_VERSION 1
#define FCGI_BEGIN_REQUEST 1
#define FCGI_PARAMS 4
#define FCGI_STDIN 5
#define FCGI_STDOUT 6
#define FCGI_END_REQUEST 3

static ssize_t read_n(int fd, void *b, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)b + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static int contains(const char *hay, size_t n, const char *needle) {
    size_t k = strlen(needle);
    if (k == 0 || k > n) return 0;
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(hay + i, needle, k) == 0) return 1;
    return 0;
}

/* Minimal FCGI backend: accept one connection, discard the request frames,
 * stream a 100KB+ HTTP response as STDOUT records, then END_REQUEST. */
static void fcgi_server(const char *path, int ready_fd) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    bind(s, (struct sockaddr *)&a, sizeof a);
    listen(s, 1);
    char b = 1;
    (void)write(ready_fd, &b, 1); /* signal: listening */
    int c = accept(s, NULL, NULL);

    /* discard frames until STDIN empty */
    for (;;) {
        unsigned char h[8];
        if (read_n(c, h, 8) < 0) break;
        int type = h[1];
        int len = (h[4] << 8) | h[5];
        int pad = h[6];
        char tmp[65536];
        int left = len;
        while (left > 0) {
            int k = (int)read(c, tmp, left > (int)sizeof tmp ? (int)sizeof tmp : left);
            if (k <= 0) break;
            left -= k;
        }
        left = pad;
        while (left > 0) {
            int k = (int)read(c, tmp, left > (int)sizeof tmp ? (int)sizeof tmp : left);
            if (k <= 0) break;
            left -= k;
        }
        if (type == FCGI_STDIN && len == 0) break;
    }

    /* build a large HTTP response (>64KB) */
    enum { BODY = 100000 };
    char *resp = malloc((size_t)BODY + 256);
    int hlen = sprintf(resp, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                             "Connection: close\r\n\r\n", BODY);
    memset(resp + hlen, 'A', BODY);
    int total = hlen + BODY;
    int off = 0;
    while (off < total) {
        int chunk = total - off;
        if (chunk > 65535) chunk = 65535;
        unsigned char fh[8] = { FCGI_VERSION, FCGI_STDOUT, 0, 1,
                                (unsigned char)((chunk >> 8) & 0xff),
                                (unsigned char)(chunk & 0xff), 0, 0 };
        (void)write(c, fh, 8);
        (void)write(c, resp + off, (size_t)chunk);
        off += chunk;
    }
    unsigned char eh[8] = { FCGI_VERSION, FCGI_END_REQUEST, 0, 1, 0, 8, 0, 0 };
    unsigned char eb[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    (void)write(c, eh, 8);
    (void)write(c, eb, 8);
    free(resp);
    close(c);
    close(s);
    unlink(path);
}

typedef struct { int fd; char *buf; size_t cap; size_t len; } reader_arg;

static void *reader_thread(void *p) {
    reader_arg *a = (reader_arg *)p;
    while (a->len < a->cap) {
        ssize_t r = read(a->fd, a->buf + a->len, a->cap - a->len);
        if (r <= 0) break;
        a->len += (size_t)r;
    }
    return NULL;
}

int main(void) {
    char path[256];
    snprintf(path, sizeof path, "/tmp/agfcgi_test_%d.sock", (int)getpid());
    unlink(path);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }
    int sync[2];
    if (pipe(sync) < 0) {
        perror("pipe");
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(sv[1]);      /* child keeps sv[0]; client pair unused here */
        close(sync[0]);
        fcgi_server(path, sync[1]);
        _exit(0);
    }
    /* parent: keep sv[0] for the reader thread, hand sv[1] to forward_to_fcgi */
    close(sync[1]);
    char b;
    if (read(sync[0], &b, 1) != 1) {
        fprintf(stderr, "FAIL: backend never became ready\n");
        return 1;
    }

    HttpRequest req;
    memset(&req, 0, sizeof req);
    strcpy(req.method, "GET");
    strcpy(req.path, "/react/foo");
    req.content_length = 0;

    char in[200000];
    reader_arg ra = { sv[0], in, sizeof in, 0 };
    pthread_t tid;
    if (pthread_create(&tid, NULL, reader_thread, &ra) != 0) {
        perror("pthread_create");
        return 1;
    }

    int body_bytes = 0;
    int rc = forward_to_fcgi(path, &req, "127.0.0.1", sv[1], &body_bytes);
    close(sv[1]); /* signal EOF to the reader thread */
    pthread_join(tid, NULL);

    if (rc < 100) {
        fprintf(stderr, "FAIL: forward_to_fcgi returned %d (expected >=100)\n", rc);
        return 1;
    }
    /* The access log's %b must count body bytes only, and must not be the
     * hardcoded 0 it recorded for every relayed response: the relay sees
     * whole messages, so locating the head is its own job. */
    if (body_bytes <= 0 || body_bytes >= (int)ra.len) {
        fprintf(stderr, "FAIL: body byte count %d out of range (%zu relayed total)\n",
                body_bytes, ra.len);
        return 1;
    }
    printf("forward_to_fcgi body bytes: %d of %zu relayed\n", body_bytes, ra.len);
    if (ra.len < (size_t)100000) {
        fprintf(stderr, "FAIL: only %zu bytes relayed (old 64KB cap would truncate)\n", ra.len);
        return 1;
    }
    if (!contains(in, ra.len, "HTTP/1.1 200 OK")) {
        fprintf(stderr, "FAIL: 200 status not relayed\n");
        return 1;
    }
    printf("forward_to_fcgi streamed %zu bytes (>=100000), status %d OK\n", ra.len, rc);
    printf("\nALL forward_to_fcgi STREAM TESTS PASSED\n");
    return 0;
}
