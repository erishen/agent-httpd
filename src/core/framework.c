/* Framework layer: the public embedding API declared in src/agenthttpd.h.
 *
 * Three pieces live here:
 *   1. The custom-route table + dispatch hook (called from process_request
 *      before the built-in CGI/static gate).
 *   2. The exec-tool wrapper (fork + /bin/sh + JSON over pipes), following
 *      the same child-reaping discipline as tools.c's fetch tool.
 *   3. agenthttpd_run(): the full startup sequence that used to live in
 *      main.c — config defaults, signal installation, registry/agent
 *      initialization, socket creation, worker pool or fork-per-connection
 *      accept loop. main.c now just parses argv into an agenthttpd_config
 *      and calls this.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "internal.h"
#include "agenthttpd.h"
#include "agent.h"
#include "llm.h"
#include "mcp.h"
#include "metrics.h"
#include "minijson.h"
#include "router.h"
#include "session.h"
#include "skills.h"
#include "tools.h"

/* ---- configuration globals (moved from main.c; extern in internal.h) ---- */

char g_web_root_real[MAX_PATH_SIZE];
char g_cgi_bin_real[MAX_PATH_SIZE];
int g_server_port = 0;
int g_no_directory_listing = 0;
int g_vite_upstream_port = 0;
char g_react_sock[MAX_PATH_SIZE];
/* g_log_fp is defined in http.c alongside the log writers. */
char g_log_path[MAX_PATH_SIZE];

/* ---- custom routes ---- */

#define FRAMEWORK_ROUTES_MAX 32

typedef struct {
    char method[16];
    char path[MAX_PATH_SIZE];
    int (*fn)(HttpRequest *request, HttpResponse *response);
} FrameworkRoute;

static FrameworkRoute g_routes[FRAMEWORK_ROUTES_MAX];
static int g_route_count = 0;
static int g_framework_started = 0; /* registration closes when run() begins */

int agenthttpd_route(const char *method, const char *path,
                     int (*fn)(HttpRequest *request, HttpResponse *response)) {
    if (!method || !path || !fn) return -1;
    if (g_framework_started) return -1;
    if (g_route_count >= FRAMEWORK_ROUTES_MAX) return -1;
    FrameworkRoute *r = &g_routes[g_route_count];
    if (strlen(method) >= sizeof(r->method) ||
        utf8_valid_prefix(path, MAX_PATH_SIZE) != strlen(path)) {
        return -1;
    }
    strcpy(r->method, method);
    strcpy(r->path, path);
    r->fn = fn;
    g_route_count++;
    return 0;
}

/* Exact match, or prefix match when the pattern ends with '*'. The request
 * path may still carry a query string (see the /health check in
 * process_request), so comparison stops at '?'. */
static int route_path_matches(const char *pattern, const char *path) {
    char clean[MAX_PATH_SIZE];
    const char *q = strchr(path, '?');
    size_t n = q ? (size_t)(q - path) : strlen(path);
    if (n >= sizeof(clean)) return 0;
    memcpy(clean, path, n);
    clean[n] = '\0';
    size_t plen = strlen(pattern);
    if (plen && pattern[plen - 1] == '*') {
        return strncmp(clean, pattern, plen - 1) == 0;
    }
    return strcmp(clean, pattern) == 0;
}

/* Returns 1 when a registered route handled the request (response filled),
 * 0 when nothing matched (or the handler fell through with -1). */
int framework_route_dispatch(HttpRequest *request, HttpResponse *response) {
    for (int i = 0; i < g_route_count; i++) {
        const FrameworkRoute *r = &g_routes[i];
        if (strcmp(r->method, "*") != 0 && strcmp(r->method, request->method) != 0) {
            continue;
        }
        if (!route_path_matches(r->path, request->path)) continue;
        return r->fn(request, response) == 0 ? 1 : 0;
    }
    return 0;
}

/* ---- exec tools ---- */

#define TOOL_EXEC_OUT_MAX (256 * 1024)

static void exec_tool_run(void *data, const char *args_json,
                          const char *session_id, sbuf *out) {
    (void)session_id;
    const char *command = (const char *)data;
    int in_pipe[2], out_pipe[2];

    if (pipe(in_pipe) < 0) {
        sb_str(out, "error: pipe failed");
        return;
    }
    if (pipe(out_pipe) < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        sb_str(out, "error: pipe failed");
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        sb_str(out, "error: fork failed");
        return;
    }
    if (pid == 0) {
        /* Child: stdin = args JSON, stdout+stderr = result stream. */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        /* Same scrub as the MCP / CGI children: third-party tool code must
         * never see the LLM credentials. */
        unsetenv("LLM_API_KEY");
        unsetenv("LLM_API_URL");
        unsetenv("LLM_MODEL");
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    /* Parent: feed the arguments, then drain the result to EOF. */
    close(in_pipe[0]);
    close(out_pipe[1]);
    size_t args_len = args_json ? strlen(args_json) : 0;
    size_t sent = 0;
    while (sent < args_len) {
        ssize_t w = write(in_pipe[1], args_json + sent, args_len - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            break; /* EPIPE: the tool exited without reading stdin — fine */
        }
        sent += (size_t)w;
    }
    close(in_pipe[1]);

    char *buf = NULL;
    size_t blen = 0;
    char chunk[4096];
    int truncated = 0;
    for (;;) {
        ssize_t r = read(out_pipe[0], chunk, sizeof chunk);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        if (blen + (size_t)r > TOOL_EXEC_OUT_MAX) {
            size_t take = TOOL_EXEC_OUT_MAX - blen;
            char *nb = realloc(buf, blen + take + 1);
            if (!nb) { truncated = 1; break; }
            buf = nb;
            memcpy(buf + blen, chunk, take);
            blen += take;
            truncated = 1;
            break; /* keep draining below, but stop buffering */
        }
        char *nb = realloc(buf, blen + (size_t)r + 1);
        if (!nb) { truncated = 1; break; }
        buf = nb;
        memcpy(buf + blen, chunk, (size_t)r);
        blen += (size_t)r;
    }
    /* Drain to EOF so the child is never blocked on a full pipe. */
    while (read(out_pipe[0], chunk, sizeof chunk) > 0) {
    }
    close(out_pipe[0]);

    int status = 0;
    pid_t reaped;
    do {
        reaped = waitpid(pid, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    /* SIGCHLD is SIG_IGN process-wide (main.c / agenthttpd_run): the kernel
     * reaps the child as it exits and waitpid() comes back ECHILD with
     * `status` untouched. Only trust the exit code when we actually reaped
     * (same guard as tools.c / agent.c / cgi.c). */
    int code_known = (reaped == pid) && WIFEXITED(status);
    int code = code_known ? WEXITSTATUS(status) : -1;

    if (!buf || blen == 0) {
        if (code == 127) sb_str(out, "error: command not found");
        else if (code_known && code != 0) sb_str(out, "error: exit code ");
        else sb_str(out, "(no output)");
        if (code_known && code != 0 && code != 127) {
            char tail[16];
            snprintf(tail, sizeof tail, "%d", code);
            sb_str(out, tail);
        }
        free(buf);
        return;
    }
    buf[blen] = '\0';
    sb_str(out, buf);
    if (truncated) sb_str(out, "\n[output truncated]");
    if (code_known && code != 0) {
        char tail[64];
        snprintf(tail, sizeof tail, "\n[exit code %d]", code);
        sb_str(out, tail);
    }
    free(buf);
}

int agenthttpd_tool_exec(const char *name, const char *desc,
                         const char *params_json, const char *command) {
    if (!command) return -1;
    return agenthttpd_tool(name, desc, params_json, exec_tool_run,
                           (void *)command);
}

int agenthttpd_tool(const char *name, const char *desc, const char *params_json,
                    ToolFn fn, void *data) {
    if (g_framework_started) return -1;
    return tools_register(name, desc, params_json, fn, data);
}

/* ---- lifecycle ---- */

static void *session_prune_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(24 * 60 * 60);
        session_prune_old(30.0);
    }
    return NULL;
}

int agenthttpd_run(const agenthttpd_config *cfg) {
    agenthttpd_config c;
    memset(&c, 0, sizeof(c));
    if (cfg) c = *cfg;
    if (c.port <= 0) c.port = DEFAULT_PORT;
    if (c.workers < 0 || c.workers > FDPASS_MAX_N) c.workers = DEFAULT_WORKERS;
    if (!c.docroot) c.docroot = WEB_ROOT;
    if (!c.cgi_bin) c.cgi_bin = CGI_BIN;
    if (!c.access_log) c.access_log = LOG_FILE;
    if (!c.auth_realm) c.auth_realm = DEFAULT_AUTH_REALM;

    g_server_port = c.port;
    g_no_directory_listing = c.no_directory_listing ? 1 : 0;
    g_vite_upstream_port = c.vite_upstream_port;
    if (c.react_socket) {
        strncpy(g_react_sock, c.react_socket, sizeof(g_react_sock) - 1);
    }
    /* allow timeout overrides from the environment (used by the test-suite) */
    g_request_timeout_seconds = env_int("REQUEST_TIMEOUT_SECONDS", g_request_timeout_seconds);
    g_cgi_timeout_seconds = env_int("CGI_TIMEOUT_SECONDS", g_cgi_timeout_seconds);
    g_cgi_body_tmp_threshold = env_int("CGI_BODY_TMP_THRESHOLD", g_cgi_body_tmp_threshold);
    g_rate_limit_rps = c.rate_limit_rps;
    g_rate_limit_rps = env_int("RATE_LIMIT_RPS", g_rate_limit_rps);
    /* Trusted reverse-proxy subnets for X-Forwarded-For-aware rate limiting.
     * Must be set before the worker pool forks (rate_limit_init + workers
     * inherit the parsed table). Unset = trust nobody. */
    {
        const char *tp = getenv("RATE_LIMIT_TRUSTED_PROXIES");
        if (tp) rate_limit_set_trusted_proxies(tp);
    }
    snprintf(g_log_path, sizeof(g_log_path), "%s", c.access_log);

    if (c.htpasswd) {
        strncpy(g_auth_file, c.htpasswd, sizeof(g_auth_file) - 1);
        if (load_htpasswd(g_auth_file) < 0) {
            return 1;
        }
    }
    strncpy(g_auth_realm, c.auth_realm, sizeof(g_auth_realm) - 1);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    if (!realpath(c.docroot, g_web_root_real)) {
        strncpy(g_web_root_real, c.docroot, sizeof(g_web_root_real) - 1);
    }
    if (!realpath(c.cgi_bin, g_cgi_bin_real)) {
        strncpy(g_cgi_bin_real, c.cgi_bin, sizeof(g_cgi_bin_real) - 1);
    }

    g_log_fp = fopen(g_log_path, "a");
    if (!g_log_fp) {
        fprintf(stderr, "warning: cannot open access log %s (continuing without logging)\n",
                g_log_path);
    } else {
        /* Access log holds client IPs + full URLs; keep it private. */
        (void)fchmod(fileno(g_log_fp), 0600);
    }

    /* Rate limiter shared memory must exist before any worker is forked. */
    rate_limit_init();
    /* Metrics table likewise: master + workers share one mmap. */
    metrics_init();
    /* Agent chat token semaphore likewise: created here so every worker /
     * forked handler inherits the pipe fds. */
    agent_init();
    /* Startup hygiene: drop session transcripts older than 30 days, then
     * keep doing it daily for the lifetime of the process. */
    session_prune_old(30.0);
    pthread_t prune_tid;
    if (pthread_create(&prune_tid, NULL, session_prune_thread, NULL) == 0) {
        pthread_detach(prune_tid);
    }
    /* Optional llm-router catalog sync (no-op when LLM_API_URL is not a
     * router), then the tool registry + skill index + MCP servers. All
     * pre-fork, so the live proxies are inherited by workers. */
    router_sync_all();
    skills_init();
    tools_init();
    mcp_init();

    /* Registration closes here: routes and tools are pre-fork snapshots. */
    g_framework_started = 1;

    int server_fd = create_server_socket(c.port);
    if (server_fd < 0) {
        if (g_log_fp) fclose(g_log_fp);
        return 1;
    }
    int fcgi_fd = -1;
    if (c.fcgi_socket) {
        fcgi_fd = create_fastcgi_listener(c.fcgi_socket);
        if (fcgi_fd < 0) {
            close(server_fd);
            if (g_log_fp) fclose(g_log_fp);
            return 1;
        }
    }

    if (g_vite_upstream_port > 0) {
        fprintf(stderr,
                "WARNING: dev Vite proxy is ENABLED (upstream 127.0.0.1:%d).\n"
                "It serves raw development sources and framework internals.\n"
                "This mode is for local development only - never expose this\n"
                "instance to a public network while -v is active.\n",
                g_vite_upstream_port);
    }
    printf("AgentHTTPD server started on port %d\n", c.port);
    printf("Web root: %s\n", g_web_root_real);
    printf("CGI bin: %s\n", g_cgi_bin_real);
    printf("Log file: %s\n", g_log_path);

    int workers = c.workers;
    int pool_mode = 0;
    if (workers > 0 && start_worker_pool(workers, server_fd) < 0) {
        fprintf(stderr, "worker pool failed, falling back to fork-per-connection\n");
        workers = 0;
    } else if (workers > 0) {
        pool_mode = 1;
    }
    if (pool_mode) {
        printf("Model: master event loop + %d slow-path workers\n", workers);
    } else {
        printf("Model: fork-per-connection\n");
    }
    printf("Press Ctrl+C to stop\n\n");

    if (pool_mode) {
        /* Fast requests are served in the master's event loop; CGI / chat /
         * /react-relay / body-carrying requests go to the worker pool. */
        event_loop(server_fd, fcgi_fd);
    } else {
    while (g_server_running) {
        struct timeval tv;
        fd_set rfds;
        int nfds = server_fd + 1;

        if (g_reopen_log) {
            if (g_log_fp) fclose(g_log_fp);
            g_log_fp = fopen(g_log_path, "a");
            g_reopen_log = 0;
        }

        if (g_resync) {
            g_resync = 0;
            /* SIGHUP reload: pull the router catalog again and rebuild the
             * skill index + MCP tool registry. In fork-per-connection mode
             * every child forked afterwards inherits the fresh state. */
            printf("Catalog resync (SIGHUP): router sync + index rebuild...\n");
            fflush(stdout);
            router_sync_all();
            skills_init();
            mcp_init();
            printf("Catalog resync done: %d skill(s), %d tool(s), %d mcp server(s).\n",
                   skills_count(), tools_count(), mcp_server_count());
            if (workers > 0) {
                printf("note: prefork workers keep their startup catalog; "
                       "restart to refresh them\n");
            }
            fflush(stdout);
        }

        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        if (fcgi_fd >= 0) {
            FD_SET(fcgi_fd, &rfds);
            if (fcgi_fd >= nfds) nfds = fcgi_fd + 1;
        }
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(nfds, &rfds, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            continue;
        }
        if (FD_ISSET(server_fd, &rfds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                if (errno != EINTR) perror("accept");
            } else {
                /* workers > 0 never reaches this loop: pool_mode dispatches
                 * through event_loop() above, which owns the slow-path queue
                 * and hands fds to the prefork pool. This select() loop only
                 * serves the fork-per-connection model. (Do not reinstate a
                 * pool branch here: token_r is O_NONBLOCK, so the blocking
                 * pool_claim_slot() contract would have to be honoured via
                 * poll() — see worker.c.) */
                fflush(stdout); /* don't clone the parent's buffered banner */
                pid_t pid = fork();
                if (pid == 0) {
                    close(server_fd);
                    if (fcgi_fd >= 0) close(fcgi_fd);
                    handle_client(client_fd, &client_addr);
                    exit(0);
                } else if (pid > 0) {
                    close(client_fd);
                } else {
                    perror("fork");
                    close(client_fd);
                }
            }
        }
        if (fcgi_fd >= 0 && FD_ISSET(fcgi_fd, &rfds)) {
            struct sockaddr_un client_sa;
            socklen_t client_len = sizeof(client_sa);
            int client_fd = accept(fcgi_fd, (struct sockaddr *)&client_sa, &client_len);
            if (client_fd < 0) {
                if (errno != EINTR) perror("fcgi accept");
                continue;
            }
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
                close(fcgi_fd);
                close(server_fd);
                fastcgi_handle_connection(client_fd);
                exit(0);
            } else if (pid > 0) {
                close(client_fd);
            } else {
                perror("fcgi fork");
                close(client_fd);
            }
        }
    }
    }

    printf("\nShutting down server...\n");
    pool_shutdown();
    close(server_fd);
    if (fcgi_fd >= 0) {
        close(fcgi_fd);
        if (c.fcgi_socket) unlink(c.fcgi_socket);
    }
    if (g_log_fp) fclose(g_log_fp);

    return 0;
}
