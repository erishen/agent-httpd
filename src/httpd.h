#ifndef HTTPD_H
#define HTTPD_H

#include <sys/types.h> /* off_t */
#include <netinet/in.h> /* struct sockaddr_in in shared prototypes */

#define MAX_REQUEST_SIZE 65536
#define MAX_RESPONSE_SIZE 65536
#define MAX_PATH_SIZE 1024
#define MAX_STREAMED_SIZE 65536
#define SERVER_VERSION "AgentHTTPD/1.1"
#define DEFAULT_PORT 18080
#define CGI_BODY_TMP_THRESHOLD_DEFAULT 524288
#define CGI_TIMEOUT_SECONDS_DEFAULT 30
#define REQUEST_TIMEOUT_SECONDS_DEFAULT 30
#define CGI_TMP_DIR "/tmp"

/* Keep-alive limits (RFC 7230 section 6.3): max requests on one connection
 * and idle time waiting for the next pipelined request. */
#define MAX_KEEPALIVE_REQUESTS 100
#define KEEPALIVE_TIMEOUT_SECONDS 5

/* Basic Auth (-a htpasswd): default challenge realm when -r is not given. */
#define DEFAULT_AUTH_REALM "agent-httpd"

/* Prefork worker pool (-w N): N children are pre-spawned and serve
 * connections handed over by the parent over a UNIX socketpair
 * (SCM_RIGHTS); the parent keeps accept() duty exclusively. The
 * concurrency cap is a pipe used as a counting semaphore: it starts with
 * N token bytes (N = free workers) - the parent blocks on a token before
 * dispatching, and each worker writes its token back when done. Portable
 * everywhere (unnamed POSIX sems are unimplemented on macOS), no shm.
 * 0 workers = classic fork-per-connection (default). */
#define DEFAULT_WORKERS 8

/* Spawn n workers serving connections handed over by the parent.
 * Returns 0 on success, -1 on failure (nothing is left running). */
int start_worker_pool(int n, int server_fd);

/* Parent-side dispatch API for the accept loop:
 * pool_claim_slot blocks until a worker frees up (returns 0 when shutdown
 * interrupts the wait); pool_dispatch_fd hands the accepted fd to that
 * worker; on send failure pool_return_token releases the slot again. */
int pool_claim_slot(char *tok_out);
int pool_claim_slot_nb(char *tok_out);
int pool_token_fd(void);
int pool_dispatch_fd(int client_fd);
void pool_return_token(char tok);
/* Reap the pool's workers and release its fds at shutdown; no-op when the
 * pool never started. */
void pool_shutdown(void);

/* Guard rails (overridable at runtime for tests):
 * - CGI_BODY_TMP_THRESHOLD: CGI response bodies larger than this are spooled
 *   to a temp file and streamed (same mechanism as big static files).
 * - CGI_TIMEOUT_SECONDS / REQUEST_TIMEOUT_SECONDS: after this much silence a
 *   CGI child is killed (504) / a half-sent request is dropped (408), so a
 *   stuck client or script cannot pin a forked handler forever.
 * - POST body reads and the resident-backend relay reuse the request timeout. */
extern int g_cgi_body_tmp_threshold;
extern int g_cgi_timeout_seconds;
extern int g_request_timeout_seconds;

typedef struct {
    char method[16];
    char path[MAX_PATH_SIZE];
    char protocol[16];
    char host[256];
    char user_agent[512];
    char referer[512];
    char accept[512];
    char accept_encoding[256];
    char connection[32];
    char content_type[128];
    char authorization[512];
    char if_none_match[256];
    char if_modified_since[64];
    char expect[32];
    char range[64]; /* raw Range: header value (bytes=N-M single range) */
    char remote_addr[64]; /* client IP, dotted quad (CGI REMOTE_ADDR) */
    int content_length;
    int chunked;       /* Transfer-Encoding: chunked framing (RFC 9110 8.7) */
    int te_unsupported; /* a request transfer coding we cannot frame */
    char *body;
} HttpRequest;

typedef struct {
    int status_code;
    char status_text[64];
    char content_type[128];
    char content_encoding[64];
    char location[512];
    int retry_after;
    char *body;
    int body_length;
    /* Bodies larger than MAX_STREAMED_SIZE are streamed from `stream_path`
     * after the headers (static: real disk file; CGI: temp file owned by the
     * response, removed after send). */
    char *stream_path;
    int stream_is_temp;
    /* Byte offset where the streamed body starts (0 = whole file; a
     * 206 partial response streams only body_length bytes from here). */
    off_t stream_offset;
    /* 401 only: realm echoed in the WWW-Authenticate challenge. */
    const char *auth_realm;
    /* 200 static-file responses: strong (size, mtime) validator echoed by
     * If-None-Match on the next request (304 body-less revalidation). */
    char etag[80];
    /* 200 static-file responses: Last-Modified (IMF-fixdate) for clients
     * that only speak If-Modified-Since. */
    char last_modified[40];
    /* 206/416 only: Content-Range header value, e.g. "bytes 0-99/1234". */
    char content_range[80];
    /* 405/OPTIONS only: methods the resource understands, echoed as the
     * Allow header (RFC 9110 15.5.1 makes Allow mandatory on 405). */
    char allow[96];
    /* Streaming dispatchers (SSE chat) write head + body to client_fd
     * themselves; handle_client then only logs (body_length = bytes sent)
     * and closes — no response serialization, no keep-alive. */
    int handled;
} HttpResponse;

/* Basic Auth (-a <htpasswd-file>, -r <realm>). Empty g_auth_file disables
 * auth entirely. Entries are "user:secret" where secret is either plaintext
 * or a crypt(3) hash (DES/MD5/SHA family, auto-detected); mismatching lines
 * are skipped so a malformed file can never turn into "allow all". */
extern char g_auth_file[MAX_PATH_SIZE];
extern char g_auth_realm[128];
int load_htpasswd(const char *path);
/* Verify one Authorization: header value. Returns 1 when valid. */
int check_basic_auth(const char *header_value);

/* Per-IP fixed-window rate limiting (shared memory across workers).
 * g_rate_limit_rps == 0 disables. rate_limit_init() must run once in the
 * PARENT before workers are forked; rate_limit_allow(ip) is process-safe. */
extern int g_rate_limit_rps;
void rate_limit_init(void);
int rate_limit_allow(unsigned int ip);

/* Shared request pipeline: method check + CGI/static dispatch.
 * Fills `response`; response->body is heap-owned by caller.
 * `client_fd` is polled for early-close while a CGI runs (pass -1 when the
 * client is not on the line, e.g. the FastCGI server path). */
int process_request(HttpRequest *request, HttpResponse *response, int client_fd);

/* Fast-path predicate for the master event loop: true only for requests the
 * loop can serve without ever blocking (GET/HEAD, no body, not a CGI path,
 * not the native chat route, not the /react FastCGI relay). Everything else
 * is handed off to a blocking pool worker. */
int is_fast_request(const HttpRequest *request);

/* Dev mode (-v <port>): true when the request must be proxied to the Vite
 * development server (client module transforms under /@ and /src/, the dev
 * SSR pages under /react/ except the native chat route). These are served by
 * the blocking worker pool via a plain HTTP forward. */
int is_vite_proxy_route(const HttpRequest *request);

/* True when the raw header block asks for a WebSocket upgrade (HMR tunnel). */
int is_websocket_upgrade(const char *raw, size_t len);

/* Serialize an HttpResponse into a raw HTTP/1.1 message. Returns bytes sent.
 * keep_alive selects the Connection header value (1 = keep-alive). */
int build_response(const HttpResponse *response, int head_only, int keep_alive, char *raw_response, int *response_len);

/* Serve one accepted client connection to completion (keep-alive loop).
 * Closes client_fd before returning. */
void handle_client(int client_fd, struct sockaddr_in *client_addr);

void set_error_response(HttpResponse *response, int status_code, const char *status_text);

void log_request(const char *client_ip, const HttpRequest *request, int status_code, int bytes);

/* FCGI client: relay one request to a resident FastCGI backend and collect
 * its raw HTTP response into out_buf (out_len is in/out capacity usable).
 * Returns 0 on success. */
int forward_to_fcgi(const char *sock_path, const HttpRequest *request,
                    const char *remote_addr, char *out_buf, int *out_len);

#endif