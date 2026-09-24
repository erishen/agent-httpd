#ifndef INTERNAL_H
#define INTERNAL_H

/* 平台特性宏 (_GNU_SOURCE/_DARWIN_C_SOURCE) 由 Makefile 的 CFLAGS 按平台
 * 注入 —— 在头文件里定义会晚于首个系统头, glibc/musl 的 features.h 届时
 * 已锁死特性集, 为时已晚。 */

/* Cross-module declarations private to the agent-httpd server binary.
 * httpd.h carries the public contract (also used by fastcgi.c); everything
 * shared between the server's translation units lives here. */

#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <netinet/in.h>

#include "httpd.h"

/* Fixed server-relative paths; main() resolves the real paths at startup. */
#define WEB_ROOT "./www"
#define CGI_BIN "./cgi-bin"
#define LOG_FILE "./logs/access.log"

/* Largest worker count the fd-passing pool supports (main() validates -w). */
#define FDPASS_MAX_N 64

/* ---- configuration globals (defined in main.c) ---- */
extern char g_web_root_real[PATH_MAX]; /* realpath(WEB_ROOT) */
extern char g_views_real[PATH_MAX];    /* realpath(views); "" = off */

/* Bounded string set: always NUL-terminates; see util.c. */
void set_str(char *dst, size_t dst_size, const char *src);
/* set_str clipped to the longest valid UTF-8 prefix (never a partial char). */
void set_str_utf8(char *dst, size_t dst_size, const char *src);

/* Longest bytes of s that form a valid UTF-8 prefix and fit within max
 * (byte clipping a multi-byte sequence mangles the JSON envelope that is
 * forwarded upstream). Returns strlen(s) when it fits whole. See util.c. */
size_t utf8_valid_prefix(const char *s, size_t max);
extern char g_cgi_bin_real[PATH_MAX];  /* realpath(CGI_BIN) */
extern int g_server_port;                   /* -p, feeds CGI SERVER_PORT */
extern int g_no_directory_listing;          /* -n: return 404 for dir requests */
extern int g_vite_upstream_port;            /* -v: dev Vite port to proxy /@, /react, /src to */
extern char g_react_sock[MAX_PATH_SIZE];    /* -R: resident React backend */

/* ---- lifecycle + logging (http.c; driven by main.c) ---- */
extern volatile sig_atomic_t g_server_running; /* SIGINT/SIGTERM flag */
extern volatile sig_atomic_t g_reopen_log;     /* SIGHUP log-reopen flag */
extern volatile sig_atomic_t g_resync;         /* SIGHUP catalog-resync flag */
extern FILE *g_log_fp;                         /* access log handle */
extern char g_log_path[MAX_PATH_SIZE];         /* access log file path */
void signal_handler(int sig);

/* Worker-side graceful shutdown flag (worker.c): SIGTERM sets it; workers
 * finish the current connection, handle_client stops its keep-alive loop,
 * and the worker exits after releasing its slot. */
extern volatile sig_atomic_t g_shutdown_requested;

/* Master event loop (src/core/event.c): serves fast requests in-process and
 * hands slow ones to the prefork pool. Runs until g_server_running is
 * cleared. Returns 0 on clean exit. */
/* IPv4 and IPv6 listeners; pass -1 for the one that could not be created. */
int event_loop(int server_fd4, int server_fd6, int fcgi_fd);

/* ---- framework.c (public API declared in agenthttpd.h) ---- */
/* Custom-route dispatch: runs the agenthttpd_route() table against the
 * request before the built-in CGI/static gate. Returns 1 when the request
 * was handled, 0 to fall through. */
int framework_route_dispatch(HttpRequest *request, HttpResponse *response);

/* ---- util.c ---- */
int env_int(const char *name, int dflt);
int get_request_timeout(void);
int get_cgi_timeout(void);
int get_cgi_body_tmp_threshold(void);
void trim_whitespace(char *s);
void url_decode(char *dst, const char *src);
int path_has_dot_component(const char *path);
void html_escape(const char *src, char *dst, size_t dst_size);
const char *get_content_type(const char *path);
/* Format a peer sockaddr (IPv4 or IPv6) into buf; returns buf. */
const char *sockaddr_to_str(const struct sockaddr *sa, char *buf, size_t n);

/* ---- auth.c (check_basic_auth/load_htpasswd declared in httpd.h) ---- */
void b64_decode(const char *in, char *out, size_t out_size);

/* ---- static.c ---- */
const char *resolve_within(const char *base_real, const char *decoded_path,
                           char *out, size_t out_size);
int is_cgi_request(const char *path);
void read_file_into_response(const char *file_path, HttpResponse *response);
void compute_etag(const struct stat *st, char *out, size_t outsz);
int etag_matches(const char *header, const char *etag);
/* RFC 9110 date helpers: IMF-fixdate emission and If-Modified-Since check. */
void format_http_date(char *out, size_t outsz, time_t t);
int not_modified_since(const char *header, time_t mtime);

/* RFC 9110 14.1.1 single byte-range parser: 1 = ok (start/len clamped to
 * size), -1 = syntactically valid but unsatisfiable (416), 0 = not a
 * usable single range (caller serves the whole file). */
int parse_range(const char *header, off_t size, off_t *start, off_t *len);
int handle_directory(const char *real_path, const char *request_path,
                     HttpResponse *response);
int handle_static_file(const HttpRequest *request, HttpResponse *response);
/* Views-root static: 0 served / -1 refused / 1 absent -> try docroot. */
int handle_views_file(const HttpRequest *request, HttpResponse *response);

/* ---- cgi.c ---- */
int execute_cgi(const HttpRequest *request, HttpResponse *response,
                int client_fd);

/* ---- http.c ---- */
int parse_request(const char *raw_request, HttpRequest *request);
/* Create the IPv4 listening TCP socket (SO_REUSEADDR, backlog 128).
 * host == NULL binds INADDR_ANY (0.0.0.0), matching the historical default;
 * a non-NULL dotted-quad address (e.g. "127.0.0.1") restricts the listener
 * to that interface. Returns -1 when host is invalid or bind fails. */
int create_server_socket(const char *host, int port);
/* Create the IPv6 listening socket bound to :: with IPV6_V6ONLY (so it does
 * not also swallow IPv4; the v4 listener above owns those). host == NULL
 * binds in6addr_any; a non-NULL IPv6 literal restricts the listener.
 * Returns -1 when the host has no usable IPv6 stack (or the host is not a
 * v6 address), letting the caller serve IPv4 only. */
int create_server_socket6(const char *host, int port);

/* ---- worker.c (start_worker_pool declared in httpd.h) ---- */
/* Reap the pool's workers and release its dispatch/token fds after the
 * accept loop ends. No-op when the pool never started. */
void pool_shutdown(void);

/* ---- fastcgi.c (server side; forward_to_fcgi lives in httpd.h) ---- */
int create_fastcgi_listener(const char *sock_path);
void fastcgi_handle_connection(int fd);

#endif /* INTERNAL_H */
