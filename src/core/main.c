/* Command-line front end for the agent-httpd framework.
 *
 * All the server machinery used to live here; since the framework split it
 * is a thin client of the public embedding API (src/agenthttpd.h): parse
 * argv into an agenthttpd_config, then call agenthttpd_run(). The library
 * entry point (src/core/framework.c) owns config defaults, signals,
 * registry initialization and the accept loops.
 *
 * The three deprecated/-dev flags (-F FastCGI listen, -R React relay, -v
 * Vite dev proxy) map onto the same config fields an embedded application
 * would set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"
#include "agenthttpd.h"

static void print_usage(const char *program) {
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
    printf("\nEmbedding: see src/agenthttpd.h and examples/embedded.c — the same\n");
    printf("server is available as a library (agenthttpd_run + route/tool hooks).\n");
}

int main(int argc, char *argv[]) {
    agenthttpd_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = DEFAULT_PORT;
    cfg.workers = DEFAULT_WORKERS;
    const char *log_path_arg = NULL; /* -L override, NULL = default */
    int opt;

    while ((opt = getopt(argc, argv, "p:F:R:T:w:a:r:l:L:hnv:")) != -1) {
        switch (opt) {
            case 'p':
                cfg.port = atoi(optarg);
                if (cfg.port <= 0 || cfg.port > 65535) {
                    fprintf(stderr, "Invalid port number: %s\n", optarg);
                    return 1;
                }
                break;
            case 'w':
                cfg.workers = atoi(optarg);
                if (cfg.workers < 0 || cfg.workers > FDPASS_MAX_N) {
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
                cfg.fcgi_socket = optarg;
                break;
            case 'R':
                cfg.react_socket = optarg;
                break;
            case 'a':
                cfg.htpasswd = optarg;
                break;
            case 'r':
                cfg.auth_realm = optarg;
                break;
            case 'l':
                cfg.rate_limit_rps = atoi(optarg);
                if (cfg.rate_limit_rps < 0 || cfg.rate_limit_rps > 1000000) {
                    fprintf(stderr, "Invalid rate limit: %s (0=off, 1-1000000)\n", optarg);
                    return 1;
                }
                break;
            case 'L':
                log_path_arg = optarg;
                break;
            case 'n':
                cfg.no_directory_listing = 1;
                break;
            case 'v':
                cfg.vite_upstream_port = atoi(optarg);
                if (cfg.vite_upstream_port <= 0 || cfg.vite_upstream_port > 65535) {
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

    cfg.access_log = log_path_arg; /* NULL = framework default */
    return agenthttpd_run(&cfg);
}
