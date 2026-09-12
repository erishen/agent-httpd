/* FastCGI server module for AgentHTTPD.
 * Listens on a UNIX socket and speaks the FCGI wire protocol (FastCGI,
 * spec of Dec 1996). It reconstructs a request from FCGI frames, reuses
 * the HTTP pipeline (process_request), and streams the HTTP response back
 * as a series of FCGI_STDOUT records followed by FCGI_END_REQUEST.
 *
 * This is the "server-side" of FastCGI: point nginx's fastcgi_pass at
 * this socket and AgentHTTPD answers like PHP-FPM would.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include "httpd.h"

/* Timeout overrides for tests; defaults live in util.c (see httpd.h). */
extern int g_cgi_body_tmp_threshold;
extern int g_cgi_timeout_seconds;
extern int g_request_timeout_seconds;

/* --- FCGI record types --- */
#define FCGI_VERSION 1
#define FCGI_BEGIN_REQUEST 1
#define FCGI_END_REQUEST   3
#define FCGI_PARAMS        4
#define FCGI_STDIN         5
#define FCGI_STDOUT        6
#define FCGI_RESPONDER     1
#define FCGI_REQUEST_COMPLETE 0

#define FCGI_HEADER_LEN 8
#define FCGI_PAD 8

typedef struct {
    unsigned char version;
    unsigned char type;
    unsigned char request_id[2];
    unsigned char content_length[2];
    unsigned char padding_length;
    unsigned char reserved;
} FcgHeader;

typedef struct {
    char *name;
    char *value;
} FcgParam;

/* --- wire helpers --- */

static int recv_n(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int send_n(int fd, const void *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = write(fd, (const char *)buf + sent, n - sent);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)r;
    }
    return 0;
}

static unsigned short be16(const unsigned char *p) {
    return (unsigned short)((p[0] << 8) | p[1]);
}

static void put_be16(unsigned char *p, unsigned short v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xff);
}

/* Drain the body+padding of the current record. Returns 0 on success. */
static int drain_frame_body(int fd, unsigned int content_len,
                            unsigned char padding_len) {
    char tmp[FCGI_PAD];
    while (content_len > 0) {
        size_t chunk = content_len < sizeof(tmp) ? content_len : sizeof(tmp);
        if (recv_n(fd, tmp, chunk) < 0) return -1;
        content_len -= (unsigned int)chunk;
    }
    if (padding_len > 0 && recv_n(fd, tmp, padding_len) < 0) return -1;
    return 0;
}

static int send_frame(int fd, unsigned char type, unsigned short req_id,
                      const void *data, size_t len) {
    unsigned char hdr[FCGI_HEADER_LEN];
    unsigned char pad[FCGI_PAD] = {0};
    size_t pad_len = (FCGI_PAD - (len % FCGI_PAD)) % FCGI_PAD;

    hdr[0] = FCGI_VERSION;
    hdr[1] = type;
    put_be16(hdr + 2, req_id);
    put_be16(hdr + 4, (unsigned short)len);
    hdr[6] = (unsigned char)pad_len;
    hdr[7] = 0;

    if (send_n(fd, hdr, FCGI_HEADER_LEN) < 0) return -1;
    if (len > 0 && send_n(fd, data, len) < 0) return -1;
    if (pad_len > 0 && send_n(fd, pad, pad_len) < 0) return -1;
    return 0;
}

static void param_free(FcgParam *p, int n) {
    for (int i = 0; i < n; i++) {
        free(p[i].name);
        free(p[i].value);
    }
    free(p);
}

static const char *param_get(const FcgParam *p, int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(p[i].name, name) == 0) return p[i].value;
    }
    return NULL;
}

/* Parse FCGI_PARAMS content: name/value pairs with 1- or 4-byte length
 * prefixes. Wire order per the FCGI spec is name_len value_len name value
 * — both lengths come FIRST, back to back. (An earlier version read
 * name_len name value_len value, which round-trips against our own C
 * client but silently drops every param a standard FastCGI client —
 * e.g. nginx fastcgi_pass — sends.) */
static void parse_params(const unsigned char *data, size_t len,
                         FcgParam **out, int *out_count) {
    FcgParam *params = NULL;
    int count = 0, cap = 0;
    size_t off = 0;

    while (off < len) {
        size_t name_len, value_len, name_off, value_off;

        if ((data[off] & 0x80) == 0) {
            name_len = data[off++];
        } else {
            /* 4-byte length prefix: bail out before touching data past the
             * record (a truncated FCGI_PARAMS frame must not over-read). */
            if (off + 4 > len) { off = len; break; }
            name_len = ((size_t)(data[off] & 0x7f) << 24) | ((size_t)data[off + 1] << 16) |
                       ((size_t)data[off + 2] << 8) | data[off + 3];
            off += 4;
        }
        if (off >= len) break;
        if ((data[off] & 0x80) == 0) {
            value_len = data[off++];
        } else {
            if (off + 4 > len) { off = len; break; }
            value_len = ((size_t)(data[off] & 0x7f) << 24) | ((size_t)data[off + 1] << 16) |
                        ((size_t)data[off + 2] << 8) | data[off + 3];
            off += 4;
        }
        if (off + name_len + value_len > len) break;
        name_off = off;
        off += name_len;
        value_off = off;
        off += value_len;

        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            FcgParam *grown = realloc(params, (size_t)cap * sizeof(FcgParam));
            if (!grown) break; /* keep the params parsed so far */
            params = grown;
        }
        /* on partial failure free what we already collected; count tracks it */
        params[count].name = NULL;
        params[count].value = NULL;
        params[count].name = malloc(name_len + 1);
        params[count].value = malloc(value_len + 1);
        if (!params[count].name || !params[count].value) break;
        memcpy(params[count].name, data + name_off, name_len);
        params[count].name[name_len] = '\0';
        memcpy(params[count].value, data + value_off, value_len);
        params[count].value[value_len] = '\0';
        count++;
    }
    *out = params;
    *out_count = count;
}

static long param_long(const FcgParam *p, int n, const char *name, long dflt) {
    const char *v = param_get(p, n, name);
    if (!v) return dflt;
    char *end = NULL;
    long r = strtol(v, &end, 10);
    return (end == v) ? dflt : r;
}

/* --- FCGI client: forward one agent-httpd request to a resident FastCGI
 *     backend (e.g. the resident React server). The backend answers with a
 *     complete HTTP/1.1 response inside STDOUT frames, which we stream
 *     straight to client_fd (no fixed 64KB cap). Returns the backend status
 *     code (>=100) on success, or -1 if the exchange could not be attempted. */

static void fcgi_put_len(unsigned char *buf, size_t *off, size_t len) {
    if (len < 128) {
        buf[(*off)++] = (unsigned char)len;
    } else {
        buf[(*off)] = (unsigned char)(0x80 | ((len >> 24) & 0x7f));
        buf[(*off) + 1] = (unsigned char)((len >> 16) & 0xff);
        buf[(*off) + 2] = (unsigned char)((len >> 8) & 0xff);
        buf[(*off) + 3] = (unsigned char)(len & 0xff);
        *off += 4;
    }
}

int forward_to_fcgi(const char *sock_path, const HttpRequest *request,
                    const char *remote_addr, int client_fd, int *body_bytes) {
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* BEGIN_REQUEST, role = RESPONDER */
    unsigned char begin[8] = {0};
    put_be16(begin, FCGI_RESPONDER);
    if (send_frame(fd, FCGI_BEGIN_REQUEST, 1, begin, 8) < 0) goto fail;

    /* split query string from REQUEST_URI (request.path holds the full URI) */
    char query[MAX_PATH_SIZE] = "";
    const char *q = strchr(request->path, '?');
    size_t uri_len = q ? (size_t)(q - request->path) : strlen(request->path);
    if (q) {
        snprintf(query, sizeof(query), "%s", q + 1);
    }

    char uri[MAX_PATH_SIZE];
    size_t n = uri_len < sizeof(uri) - 1 ? uri_len : sizeof(uri) - 1;
    memcpy(uri, request->path, n);
    uri[n] = '\0';

    unsigned char params[4096];
    size_t poff = 0;
    char cl[32];
    snprintf(cl, sizeof(cl), "%d", request->content_length);

    {
        const struct { const char *name; const char *value; } pv[] = {
            {"REQUEST_METHOD", request->method},
            {"REQUEST_URI", uri},
            {"QUERY_STRING", query},
            {"CONTENT_TYPE", request->content_type},
            {"CONTENT_LENGTH", cl},
            {"HTTP_HOST", request->host},
            {"HTTP_USER_AGENT", request->user_agent},
            {"REMOTE_ADDR", remote_addr},
        };
        for (size_t i = 0; i < sizeof(pv) / sizeof(pv[0]); i++) {
            if (!pv[i].value || !pv[i].value[0]) continue;
            size_t nl = strlen(pv[i].name), vl = strlen(pv[i].value);
            if (poff + 4 + nl + 4 + vl + 1 > sizeof(params)) goto fail;
            fcgi_put_len(params, &poff, nl);
            fcgi_put_len(params, &poff, vl);
            memcpy(params + poff, pv[i].name, nl);
            poff += nl;
            memcpy(params + poff, pv[i].value, vl);
            poff += vl;
        }
    }

    if (send_frame(fd, FCGI_PARAMS, 1, params, poff) < 0) goto fail;
    if (send_frame(fd, FCGI_PARAMS, 1, NULL, 0) < 0) goto fail; /* end of params */
    if (request->body && request->content_length > 0) {
        if (send_frame(fd, FCGI_STDIN, 1, request->body, (size_t)request->content_length) < 0) goto fail;
    }
    if (send_frame(fd, FCGI_STDIN, 1, NULL, 0) < 0) goto fail; /* end of stdin */

    int status = -1;
    int got_status = 0;
    int streamed_any = 0;
    long total_out = 0;    /* bytes forwarded to the client, head included */
    long header_len = -1;  /* end of the response head within the stream */
    int head_scan_done = 0;
    /* Stream STDOUT frames straight to the client until END_REQUEST. This
     * removes the old 64KB ceiling that made large SSR pages 502. The status
     * code is scraped from the first "HTTP/1.1 " bytes for logging. */
    {
        FcgHeader hdr;
        char frame[65536];
        for (;;) {
            if (recv_n(fd, &hdr, FCGI_HEADER_LEN) < 0 || hdr.version != FCGI_VERSION) {
                if (streamed_any) goto done_streamed;
                goto fail;
            }
            unsigned int len = be16(hdr.content_length);
            if (hdr.type == FCGI_STDOUT) {
                if (len > sizeof(frame)) {
                    if (streamed_any) goto done_streamed;
                    goto fail;
                }
                if (recv_n(fd, frame, len) < 0) {
                    if (streamed_any) goto done_streamed;
                    goto fail;
                }
                if (!got_status) {
                    for (size_t i = 0; i + 9 <= len; i++) {
                        if (memcmp(frame + i, "HTTP/1.1 ", 9) == 0) {
                            status = atoi((char *)frame + i + 9);
                            got_status = 1;
                            break;
                        }
                    }
                }
                /* Locate the end of the head while it is still in view: the
                 * relay forwards whole messages, but the access log wants
                 * body bytes (%b). A response head is tiny next to the 64KB
                 * frame cap, so it always arrives whole in this first frame;
                 * if it somehow does not, the log reports the whole message
                 * rather than a wrong body count. */
                if (!head_scan_done) {
                    head_scan_done = 1;
                    for (size_t i = 0; i + 4 <= len; i++) {
                        if (memcmp(frame + i, "\r\n\r\n", 4) == 0) {
                            header_len = (long)i + 4;
                            break;
                        }
                    }
                }
                size_t sent = 0;
                while (sent < len) {
                    ssize_t w = send(client_fd, frame + sent, len - sent, 0);
                    if (w <= 0) {
                        if (w < 0 && errno == EINTR) continue;
                        total_out += (long)sent;
                        goto done_streamed;
                    }
                    sent += (size_t)w;
                }
                total_out += (long)len;
                streamed_any = 1;
            } else if (len > 0) {
                char tmp[64];
                unsigned int dropped = 0;
                while (dropped < len) {
                    unsigned int chunk = len - dropped;
                    if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
                    if (recv_n(fd, tmp, chunk) < 0) goto fail;
                    dropped += chunk;
                }
            }
            if (hdr.padding_length > 0) {
                char pad[8];
                if (recv_n(fd, pad, hdr.padding_length) < 0) goto fail;
            }
            if (hdr.type == FCGI_END_REQUEST) break;
        }
    }
done_streamed:
    if (body_bytes) {
        /* Body bytes only: the log's %b must not count the head. Falls back
         * to the whole message when no head was located (non-HTTP payload
         * or a head split across frames). */
        *body_bytes = (header_len >= 0 && total_out > header_len)
                          ? (int)(total_out - header_len)
                          : (int)total_out;
    }
    close(fd);
    return got_status ? status : 200;

fail:
    if (body_bytes) *body_bytes = 0;
    close(fd);
    return -1;
}

/* --- public API --- */

int create_fastcgi_listener(const char *sock_path) {
    /* Optional ",mode" suffix (octal) widens the default 0660; parse it off
     * a local copy so the original argument stays intact for bind/unlink. */
    char path[MAX_PATH_SIZE];
    strncpy(path, sock_path, sizeof path - 1);
    path[sizeof path - 1] = '\0';
    mode_t mode = 0660;
    char *comma = strrchr(path, ',');
    if (comma) {
        mode = (mode_t)strtol(comma + 1, NULL, 8);
        *comma = '\0';
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("fcgi socket");
        return -1;
    }
    unlink(path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* sun_path is a fixed 108-byte field. A longer socket path used to be
     * silently truncated by strncpy, so bind() would claim *one* name while
     * the unlink()/chmod() below touched another — the caller then chases a
     * socket that does not exist and the mode lands on nothing. Refuse the
     * oversized path instead of guessing (GCC flags the strncpy at -O2 too). */
    size_t plen = strlen(path);
    if (plen >= sizeof(addr.sun_path)) {
        fprintf(stderr, "fcgi socket path too long (max %zu): %s\n",
                sizeof(addr.sun_path) - 1, path);
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, path, plen + 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("fcgi bind");
        close(fd);
        return -1;
    }
    /* Default 0660: only the server uid + its group may drive backend CGI.
     * In a multi-uid deployment (nginx worker under another account) the
     * caller can pass "-F path,0777" to widen — permission explosions must
     * be explicit, not the default. (mode parsed off `path` above.) */
    chmod(path, mode);
    if (listen(fd, 32) < 0) {
        perror("fcgi listen");
        close(fd);
        return -1;
    }
    printf("FastCGI listener on %s\n", path);
    return fd;
}

/* Serve one FCGI connection. Reads BEGIN_REQUEST, PARAMS (until empty),
 * STDIN (until empty), rebuilds the request, then streams the response. */
void fastcgi_handle_connection(int fd) {
    FcgHeader hdr;
    FcgParam *params = NULL;
    int nparams = 0;
    char *body = NULL;
    size_t body_len = 0, body_cap = 0;
    HttpRequest request;
    HttpResponse response;
    char resp_buf[MAX_RESPONSE_SIZE];
    int resp_len = 0, body_sent = 0;
    unsigned short req_id = 0;
    int stdin_done = 0;

    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));

    /* BEGIN_REQUEST */
    if (recv_n(fd, &hdr, FCGI_HEADER_LEN) < 0 ||
        hdr.version != FCGI_VERSION || hdr.type != FCGI_BEGIN_REQUEST) {
        goto send_end;
    }
    req_id = be16(hdr.request_id);
    if (drain_frame_body(fd, be16(hdr.content_length), hdr.padding_length) < 0) {
        goto send_end;
    }

    /* PARAMS then STDIN records */
    while (!stdin_done) {
        if (recv_n(fd, &hdr, FCGI_HEADER_LEN) < 0) goto build_request;
        unsigned int len = be16(hdr.content_length);

        if (hdr.type == FCGI_PARAMS) {
            if (len == 0) {
                if (drain_frame_body(fd, 0, hdr.padding_length) < 0) goto send_end;
                continue;
            }
            unsigned char *buf = malloc(len);
            if (!buf) goto send_end;
            if (recv_n(fd, buf, len) < 0 ||
                drain_frame_body(fd, 0, hdr.padding_length) < 0) {
                free(buf);
                goto send_end;
            }
            parse_params(buf, len, &params, &nparams);
            free(buf);
            continue;
        }

        if (hdr.type == FCGI_STDIN) {
            if (len == 0) {
                drain_frame_body(fd, 0, hdr.padding_length);
                stdin_done = 1;
                break;
            }
            if (body_len + len > MAX_REQUEST_SIZE) {
                body_len = (size_t)-1; /* oversized guard */
                drain_frame_body(fd, len, hdr.padding_length);
                break;
            }
            if (body_len + len > body_cap) {
                size_t nc = body_cap ? body_cap * 2 : 1024;
                while (nc < body_len + len) nc *= 2;
                char *nb = realloc(body, nc);
                if (!nb) goto build_request;
                body = nb;
                body_cap = nc;
            }
            if (recv_n(fd, body + body_len, len) < 0) goto build_request;
            body_len += len;
            drain_frame_body(fd, 0, hdr.padding_length);
            continue;
        }

        /* unexpected record type: try to recover without an infinite loop */
        drain_frame_body(fd, len, hdr.padding_length);
        break;
    }

build_request:
    if (body_len == (size_t)-1) body_len = 0; /* oversized body discarded */

    /* populate the request from FastCGI parameters */
    {
        const char *m = param_get(params, nparams, "REQUEST_METHOD");
        const char *uri = param_get(params, nparams, "REQUEST_URI");
        strncpy(request.method, m ? m : "GET", sizeof(request.method) - 1);
        strncpy(request.path, uri ? uri : "/", sizeof(request.path) - 1);
        strcpy(request.protocol, "HTTP/1.1");

        const char *v;
        if ((v = param_get(params, nparams, "HTTP_HOST")))        strncpy(request.host, v, sizeof(request.host) - 1);
        if ((v = param_get(params, nparams, "HTTP_USER_AGENT")))  strncpy(request.user_agent, v, sizeof(request.user_agent) - 1);
        if ((v = param_get(params, nparams, "HTTP_REFERER")))     strncpy(request.referer, v, sizeof(request.referer) - 1);
        if ((v = param_get(params, nparams, "HTTP_ACCEPT")))      strncpy(request.accept, v, sizeof(request.accept) - 1);
        if ((v = param_get(params, nparams, "CONTENT_TYPE")))     strncpy(request.content_type, v, sizeof(request.content_type) - 1);

        long cl = param_long(params, nparams, "CONTENT_LENGTH", -1);
        /* 显式统一为 long: 三元两臂符号一致 (size_t 与 long 混用会触发
         * GCC 12 的 -Wsign-compare) */
        request.content_length = (int)(cl >= 0 ? cl : (long)body_len);
        {
            /* client IP (REMOTE_ADDR when nginx relays it) else the FCGI peer */
            const char *ra = param_get(params, nparams, "REMOTE_ADDR");
            if (ra && *ra) {
                snprintf(request.remote_addr, sizeof(request.remote_addr), "%s", ra);
            } else {
                struct sockaddr_un peer;
                socklen_t plen = sizeof(peer);
                request.remote_addr[0] = '\0';
                if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0 && peer.sun_path[0]) {
                    /* %.*s 界定拷贝长度: sun_path 可达 107 字节而 remote_addr
                     * 更短, 让 GCC 能证明不会溢出 (截断的 socket 路径仅影响日志) */
                    snprintf(request.remote_addr, sizeof(request.remote_addr),
                             "unix:%.*s", (int)sizeof(request.remote_addr) - 6,
                             peer.sun_path);
                }
            }
        }

        if (body_len > 0) {
            size_t cap = body_len + 1;
            request.body = malloc(cap);
            if (!request.body) goto send_end;
            memcpy(request.body, body, body_len);
            request.body[body_len] = '\0';
        }
    }

/* No client socket to poll here: the FastCGI peer is our only channel,
 * so pass -1 (CGI early-close detection is skipped). */
    process_request(&request, &response, -1);
    if (response.body == NULL && response.stream_path == NULL && response.status_code != 0) {
        set_error_response(&response, response.status_code, response.status_text);
    }
    if (response.content_type[0] == '\0') {
        strcpy(response.content_type, "text/html");
    }
    int head_only = (strcmp(request.method, "HEAD") == 0);
    body_sent = build_response(&response, head_only, 0, resp_buf, &resp_len);

    /* stream the HTTP response as FCGI_STDOUT records (≤64KB each) */
    {
        size_t off = 0;
        while (off < (size_t)resp_len) {
            size_t chunk = resp_len - off;
            if (chunk > 0xffff) chunk = 0xffff;
            if (send_frame(fd, FCGI_STDOUT, req_id, resp_buf + off, chunk) < 0) break;
            off += chunk;
        }
    }

    /* stream out-of-buffer static files: STDOUT record per read chunk */
    if (response.stream_path && !head_only) {
        FILE *file = fopen(response.stream_path, "rb");
        if (file) {
            char chunk[65536];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0) {
                size_t off = 0;
                while (off < n) {
                    size_t c = n - off;
                    if (c > 0xffff) c = 0xffff;
                    if (send_frame(fd, FCGI_STDOUT, req_id, chunk + off, c) < 0) break;
                    off += c;
                    body_sent += (int)c;
                }
            }
            fclose(file);
        }
    }

    {
        const char *ra = param_get(params, nparams, "REMOTE_ADDR");
        log_request(ra ? ra : "-", &request, response.status_code, body_sent);
    }

    free(request.body);
    free(response.body);
    if (response.stream_path) {
        if (response.stream_is_temp) unlink(response.stream_path);
        free(response.stream_path);
    }

send_end:
    free(body);
    param_free(params, nparams);
    {
        unsigned char end_body[8] = {0}; /* appStatus=0, protocolStatus=REQUEST_COMPLETE */
        send_frame(fd, FCGI_END_REQUEST, req_id, end_body, sizeof(end_body));
    }
    close(fd);
}