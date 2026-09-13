/* Entry point: CLI parsing, runtime initialization (docroot realpathing,
 * log, rate limiter, sockets) and the accept loop that hands each
 * connection to either the worker pool or a forked handler. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include "internal.h"
#include "metrics.h"
#include "agent.h"
#include "session.h"
#include "skills.h"
#include "mcp.h"
#include "tools.h"
#include "router.h"

char g_web_root_real[MAX_PATH_SIZE];
char g_cgi_bin_real[MAX_PATH_SIZE];
int g_server_port = DEFAULT_PORT;
int g_no_directory_listing = 0;
int g_vite_upstream_port = 0;
char g_react_sock[MAX_PATH_SIZE];

char g_log_path[MAX_PATH_SIZE];
static const char *log_path_arg = NULL; /* -L override, NULL = default */

/* Re-run session pruning once a day. The startup pass alone is not enough:
 * the server is designed to stay up for weeks (docker restart:
 * unless-stopped), and old transcripts under .data/sessions would age
 * unbounded. Runs in a detached master-process thread; forked workers
 * never inherit it (fork copies only the calling thread), so it cannot
 * race request handling. */
#define SESSION_PRUNE_INTERVAL_SEC (24 * 60 * 60)
static void *session_prune_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(SESSION_PRUNE_INTERVAL_SEC);
        session_prune_old(30.0);
    }
    return NULL;
}

void print_usage(const char *program) {
    printf("Usage: %s [options]\n", program);
    printf("Options:\n");
    printf("  -p <port>       Specify HTTP port (default: %d)\n", DEFAULT_PORT);
    printf("  -F <sock-path>  Also listen as a FastCGI backend on a UNIX socket\n");
    printf("  -R <sock-path>  Relay /react/* requests to a resident FastCGI backend\n");
    printf("  -T <seconds>    Set request and CGI timeout (default: %d)\n", REQUEST_TIMEOUT_SECONDS_DEFAULT);
    printf("  -a <htpasswd>   Require Basic Auth against an htpasswd file\n");
    printf("                  (strong crypt(3) hashes only: $5$/$6$/bcrypt;\n");
    printf("                  weak forms need AGENTHTTPD_ALLOW_WEAK_AUTH=1)\n");
    printf("  -r <realm>      Basic Auth realm (default: %s)\n", DEFAULT_AUTH_REALM);
    printf("  -w <n>          Prefork worker pool mode with n workers\n");
    printf("                  (default: fork-per-connection)\n");
    printf("  -l <rps>        Per-IP rate limit: max requests per second\n");
    printf("                  (0 = off, default; breach answers 429 + Retry-After)\n");
    printf("  -L <path>       Access log file path (default: %s)\n", LOG_FILE);
    printf("  -n              Disable directory listings (dir without index\n");
    printf("                  file answers 404; default: enabled)\n");
    printf("  -v <port>       Dev ONLY: proxy /@*, /src/* and /react/* SSR\n");
    printf("                  pages to a Vite dev server on 127.0.0.1:<port>\n");
    printf("                  (HMR WebSocket upgrades are tunnelled; serves raw\n");
    printf("                  dev sources - never expose this instance publicly)\n");
    printf("  -h              Show this help message\n");
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int fcgi_fd = -1;
    char fcgi_socket[MAX_PATH_SIZE];
    int opt;
    int workers = DEFAULT_WORKERS; /* slow-path pool; 0 = classic fork-per-connection */

    fcgi_socket[0] = '\0';
    g_react_sock[0] = '\0';

    while ((opt = getopt(argc, argv, "p:F:R:T:w:a:r:l:L:hnv:")) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                if (port <= 0 || port > 65535) {
                    fprintf(stderr, "Invalid port number: %s\n", optarg);
                    return 1;
                }
                break;
            case 'w':
                workers = atoi(optarg);
                if (workers < 0 || workers > FDPASS_MAX_N) {
                    fprintf(stderr, "Invalid worker count: %s (0-%d; 0 = fork-per-connection)\n", optarg, FDPASS_MAX_N);
                    return 1;
                }
                break;
            case 'T': {
                int t = atoi(optarg);
                if (t <= 0 || t > 86400) {
                    fprintf(stderr, "Invalid timeout: %s\n", optarg);
                    return 1;
                }
                g_request_timeout_seconds = t;
                g_cgi_timeout_seconds = t;
                break;
            }
            case 'F':
                strncpy(fcgi_socket, optarg, sizeof(fcgi_socket) - 1);
                break;
            case 'R':
                strncpy(g_react_sock, optarg, sizeof(g_react_sock) - 1);
                break;
            case 'a':
                strncpy(g_auth_file, optarg, sizeof(g_auth_file) - 1);
                if (load_htpasswd(g_auth_file) < 0) {
                    return 1;
                }
                break;
            case 'r':
                strncpy(g_auth_realm, optarg, sizeof(g_auth_realm) - 1);
                break;
            case 'l':
                g_rate_limit_rps = atoi(optarg);
                if (g_rate_limit_rps < 0 || g_rate_limit_rps > 1000000) {
                    fprintf(stderr, "Invalid rate limit: %s (0=off, 1-1000000)\n", optarg);
                    return 1;
                }
                break;
            case 'L':
                log_path_arg = optarg;
                break;
            case 'n':
                g_no_directory_listing = 1;
                break;
            case 'v':
                g_vite_upstream_port = atoi(optarg);
                if (g_vite_upstream_port <= 0 || g_vite_upstream_port > 65535) {
                    fprintf(stderr, "Invalid Vite upstream port: %s\n", optarg);
                    return 1;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    g_server_port = port;
    /* allow timeout overrides from the environment (used by the test-suite) */
    g_request_timeout_seconds = env_int("REQUEST_TIMEOUT_SECONDS", g_request_timeout_seconds);
    g_cgi_timeout_seconds = env_int("CGI_TIMEOUT_SECONDS", g_cgi_timeout_seconds);
    g_cgi_body_tmp_threshold = env_int("CGI_BODY_TMP_THRESHOLD", g_cgi_body_tmp_threshold);
    g_rate_limit_rps = env_int("RATE_LIMIT_RPS", g_rate_limit_rps);
    snprintf(g_log_path, sizeof(g_log_path), "%s", log_path_arg ? log_path_arg : LOG_FILE);

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

    if (!realpath(WEB_ROOT, g_web_root_real)) {
        strncpy(g_web_root_real, WEB_ROOT, sizeof(g_web_root_real) - 1);
    }
    if (!realpath(CGI_BIN, g_cgi_bin_real)) {
        strncpy(g_cgi_bin_real, CGI_BIN, sizeof(g_cgi_bin_real) - 1);
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
    /* Optional llm-router catalog sync: materializes the router's skills
     * under skills/router/ and its MCP spawns into
     * .data/mcp-servers-router.json, so skills_init/mcp_init below pick
     * them up. No-op when LLM_API_URL is not a router. */
    router_sync_all();
    /* Tool registry + skill index + MCP servers (each MCP server spawns a
     * one-time tools/list probe; must all happen pre-fork so the live
     * proxies are inherited by workers). */
    skills_init();
    tools_init();
    mcp_init();

    int server_fd = create_server_socket(port);
    if (server_fd < 0) {
        if (g_log_fp) fclose(g_log_fp);
        return 1;
    }
    if (fcgi_socket[0]) {
        fcgi_fd = create_fastcgi_listener(fcgi_socket);
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
    printf("AgentHTTPD server started on port %d\n", port);
    printf("Web root: %s\n", g_web_root_real);
    printf("CGI bin: %s\n", g_cgi_bin_real);
    printf("Log file: %s\n", g_log_path);

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
            } else if (workers > 0) {
                /* pool mode: block until a worker frees up (read 1 token),
                 * then hand the fd over; parent closes its copy after send */
                char tok = 0;
                if (pool_claim_slot(&tok)) {
                    if (pool_dispatch_fd(client_fd) < 0) {
                        perror("fd dispatch");
                        pool_return_token(tok); /* slot back */
                        close(client_fd);
                    } else {
                        close(client_fd);
                    }
                } else {
                    close(client_fd); /* shutting down mid-dispatch */
                }
            } else {
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
            } else {
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
    }

    printf("\nShutting down server...\n");
    pool_shutdown();
    close(server_fd);
    if (fcgi_fd >= 0) {
        close(fcgi_fd);
        if (fcgi_socket[0]) unlink(fcgi_socket);
    }
    if (g_log_fp) fclose(g_log_fp);

    return 0;
}
