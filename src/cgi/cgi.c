/* CGI/1.1 execution (RFC 3875): fork+exec of scripts under /cgi-bin,
 * request body piping with timeouts, response-header parsing (Content-Type,
 * Location redirect, Status override), REMOTE_USER for authenticated
 * scripts, and temp-file spooling of oversized output. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <dirent.h>

#include "internal.h"
#include "metrics.h"

extern char **environ;

/* The httpd process holds the client socket, its listening sockets, the
 * FastCGI/Unix sockets, MCP/router pipes and every other descriptor it
 * opened. fork() duplicates all of them into the CGI child. Closing just the
 * four pipe ends here used to leave client_fd (and the listeners) open
 * across exec: a script could write straight to the client socket, or keep
 * a listener open after the parent tried to restart. A CGI needs nothing
 * beyond stdin/stdout/stderr, so drop the rest. */
static void cgi_close_inherited_fds(void) {
#ifdef __linux__
    /* Fast and exact: /proc/self/fd lists only descriptors that are open. */
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        int dfd = dirfd(d);
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            char *end = NULL;
            long v = strtol(de->d_name, &end, 10);
            if (end == de->d_name || *end != '\0' || v < 3 || v == dfd) continue;
            close((int)v);
        }
        closedir(d);
        return;
    }
#endif
    /* Portable fallback (macOS/BSD, or a container without /proc): bounded
     * loop over the descriptor space. 4096 covers the CI runners' default
     * RLIMIT_NOFILE; Linux normally takes the exact /proc path above. */
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
    for (int fd = 3; fd < (int)maxfd; fd++) close(fd);
}

/* CGIs are third-party scripts. They need the RFC 3875 metavariables and a
 * few runtime basics to start an interpreter - and nothing else. The parent
 * carries LLM_API_KEY, LLM_API_URL, ROUTER_API_URL, MCP_SERVERS,
 * AUTH_SESSION_FILE, SQLITE_DB and friends; unsetting four names by hand
 * left all of those inheritable. execve with a filtered environ is
 * fail-closed: a secret added to the parent later stays invisible unless
 * somebody deliberately adds its key here. HTTP_PROXY/HTTPS_PROXY are
 * intentionally NOT on the list (they often embed credentials). */
static const char *const cgi_env_allow[] = {
    "REQUEST_METHOD", "QUERY_STRING", "CONTENT_TYPE", "CONTENT_LENGTH",
    "SERVER_NAME", "SERVER_PORT", "SCRIPT_NAME", "REQUEST_URI",
    "SERVER_PROTOCOL", "GATEWAY_INTERFACE", "SERVER_SOFTWARE",
    "REMOTE_ADDR", "REMOTE_USER", "HTTP_HOST", "HTTP_USER_AGENT",
    "HTTP_X_FORWARDED_FOR",
    "PATH", "HOME", "LANG", "LC_ALL", "TMPDIR", "TZ",
    NULL
};

static int cgi_env_allowed(const char *entry) {
    const char *eq = strchr(entry, '=');
    if (!eq) return 0;
    size_t klen = (size_t)(eq - entry);
    for (int i = 0; cgi_env_allow[i]; i++) {
        const char *k = cgi_env_allow[i];
        if (strlen(k) == klen && strncmp(entry, k, klen) == 0) return 1;
    }
    return 0;
}

int execute_cgi(const HttpRequest *request, HttpResponse *response, int client_fd) {
    char script_path[MAX_PATH_SIZE];
    char decoded_path[MAX_PATH_SIZE];
    char resolved_path[MAX_PATH_SIZE];
    char *query_string = NULL;

    set_str(decoded_path, sizeof(decoded_path), request->path);

    query_string = strchr(decoded_path, '?');
    if (query_string) {
        *query_string = '\0';
        query_string++;
    }

    /* Dot-prefixed components refused, same policy as static files: a
     * ".hidden.cgi" dropped into cgi-bin must not be executable via HTTP
     * (404, not 403 - no existence disclosure). */
    if (path_has_dot_component(decoded_path)) {
        response->status_code = 404;
        strcpy(response->status_text, "Not Found");
        return -1;
    }

    if (!resolve_within(g_cgi_bin_real, decoded_path + strlen("/cgi-bin"), resolved_path, sizeof(resolved_path))) {
        response->status_code = 404;
        strcpy(response->status_text, "Not Found");
        return -1;
    }

    set_str(script_path, sizeof(script_path), resolved_path);

    struct stat st;
    if (stat(script_path, &st) < 0 || !(st.st_mode & S_IXUSR)) {
        response->status_code = 403;
        strcpy(response->status_text, "Forbidden");
        return -1;
    }

    int out_pipe[2];
    int in_pipe[2];
    if (pipe(out_pipe) < 0 || pipe(in_pipe) < 0) {
        response->status_code = 500;
        strcpy(response->status_text, "Internal Server Error");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(in_pipe[0]);
        close(in_pipe[1]);
        response->status_code = 500;
        strcpy(response->status_text, "Internal Server Error");
        return -1;
    }

    if (pid == 0) {
        close(out_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[1]);

        close(in_pipe[1]);
        dup2(in_pipe[0], STDIN_FILENO);
        close(in_pipe[0]);

        /* stdin/stdout are the pipes now; every descriptor inherited from
         * the server (client_fd, listeners, MCP/router pipes) must go. */
        cgi_close_inherited_fds();

        setenv("REQUEST_METHOD", request->method, 1);
        setenv("QUERY_STRING", query_string ? query_string : "", 1);
        setenv("CONTENT_TYPE", request->content_type, 1);
        char len_str[32];
        snprintf(len_str, sizeof(len_str), "%d", request->content_length);
        setenv("CONTENT_LENGTH", len_str, 1);
        setenv("SERVER_NAME", "localhost", 1);
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", g_server_port);
        setenv("SERVER_PORT", port_str, 1);
        setenv("SCRIPT_NAME", request->path, 1);
        setenv("REQUEST_URI", request->path, 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
        /* RFC 3875 core metavariables the scripts can rely on. */
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("SERVER_SOFTWARE", SERVER_VERSION, 1);
        setenv("REMOTE_ADDR", request->remote_addr, 1);
        setenv("HTTP_HOST", request->host, 1);
        setenv("HTTP_USER_AGENT", request->user_agent, 1);
        if (request->x_forwarded_for[0])
            setenv("HTTP_X_FORWARDED_FOR", request->x_forwarded_for, 1);
        if (g_auth_file[0]) {
            /* CGI convention: scripts learn who authenticated via
             * REMOTE_USER (value validated by check_basic_auth already). */
            char remote_user[64] = "";
            const char *ah = request->authorization;
            if (ah && strncasecmp(ah, "Basic ", 6) == 0) {
                char creds[384];
                b64_decode(ah + 6, creds, sizeof(creds));
                char *colon = strchr(creds, ':');
                if (colon) *colon = '\0';
                set_str(remote_user, sizeof(remote_user), creds);
            }
            setenv("REMOTE_USER", remote_user, 1);
        }

        /* execl() would hand the child the parent's full environ. Build an
         * explicit allowlist instead: the setenv() calls above put the CGI
         * metavariables into environ, and this keeps only those plus the
         * runtime basics - dropping credentials, proxy URLs, session/db
         * paths and any future secret by default. */
        char *child_env[64];
        size_t n = 0;
        for (char **e = environ; *e && n + 1 < sizeof(child_env) / sizeof(child_env[0]); e++) {
            if (cgi_env_allowed(*e)) child_env[n++] = *e;
        }
        child_env[n] = NULL;
        char *const child_argv[] = { script_path, NULL };
        execve(script_path, child_argv, (char *const *)child_env);
        fprintf(stderr, "CGI execution failed: %s\n", strerror(errno));
        exit(1);
    } else {
        close(out_pipe[1]);
        close(in_pipe[0]);

        if (request->body && request->content_length > 0) {
            /* Poll the pipe so a CGI that never reads stdin cannot pin us
             * forever (we would block in write() once the pipe fills). */
            int written = 0;
            int left = request->content_length;
            int to = get_cgi_timeout();
            while (left > 0 && to > 0) {
                struct timeval tv = {to, 0};
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(in_pipe[1], &wfds);
                if (select(in_pipe[1] + 1, NULL, &wfds, NULL, &tv) <= 0) {
                    break; /* timeout (or EINTR); give up, child is stuck */
                }
                int n = write(in_pipe[1], request->body + written, left);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                written += n;
                left -= n;
            }
        } else if (request->body_temp_path && request->content_length > 0) {
            /* Spooled request body: stream it into the CGI's stdin straight
             * from the temp file (same poll discipline) so a large upload
             * does not have to fit memory. */
            FILE *bf = fopen(request->body_temp_path, "rb");
            if (bf) {
                long left = request->content_length;
                int to = get_cgi_timeout();
                while (left > 0 && to > 0) {
                    struct timeval tv = {to, 0};
                    fd_set wfds;
                    FD_ZERO(&wfds);
                    FD_SET(in_pipe[1], &wfds);
                    if (select(in_pipe[1] + 1, NULL, &wfds, NULL, &tv) <= 0) {
                        break; /* timeout (or EINTR); give up, child is stuck */
                    }
                    char chunk[65536];
                    size_t want = left < (long)sizeof(chunk)
                                      ? (size_t)left : sizeof(chunk);
                    size_t got = fread(chunk, 1, want, bf);
                    if (got == 0) break; /* temp file shorter than declared */
                    int done = 0;
                    while (done < (int)got) {
                        int n = write(in_pipe[1], chunk + done,
                                      got - (size_t)done);
                        if (n < 0) {
                            if (errno == EINTR) continue;
                            done = (int)got; /* give up on this chunk */
                            left = 0;
                            break;
                        }
                        done += n;
                        left -= n;
                    }
                }
                fclose(bf);
            }
        }
        close(in_pipe[1]);

        /* Read CGI output with select + timeout: a script that hangs or a
         * client that vanished must not leak this handler process.
         * The buffer grows on demand; huge bodies are spooled to a temp
         * file and streamed afterwards (same mechanism as big static files). */
        size_t cap = MAX_RESPONSE_SIZE;
        char *output = malloc(cap);
        if (!output) {
            close(out_pipe[0]);
            waitpid(pid, NULL, 0);
            response->status_code = 500;
            strcpy(response->status_text, "Internal Server Error");
            return -1;
        }

        int total = 0;
        int killed = 0;
        int status = 0;
        int to = get_cgi_timeout();
        time_t deadline = to > 0 ? time(NULL) + to : 0;
        for (;;) {
            if (total + 1 >= (int)cap) {
                size_t ncap = cap * 2;
                char *nb = realloc(output, ncap);
                if (!nb) break; /* keep what we have */
                output = nb;
                cap = ncap;
            }
            struct timeval tv = {to > 0 ? to : 60, 0};
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(out_pipe[0], &rfds);
            int rv = select(out_pipe[0] + 1, &rfds, NULL, NULL, &tv);
            if (rv <= 0) {
                /* timeout, or interrupted; if the client is still there and a
                 * full timeout window has elapsed, report gateway timeout */
                killed = 1;
                break;
            }
            int n = read(out_pipe[0], output + total, cap - (size_t)total - 1);
            if (n <= 0) break; /* EOF or error: script is done */
            total += n;
            if (deadline && time(NULL) >= deadline) {
                killed = 1;
                break;
            }
            /* Early client close: polling the socket costs nothing and lets
             * us kill the CGI instead of streaming to a dead connection. */
            if (client_fd >= 0) {
                fd_set cfds;
                struct timeval cv = {0, 0};
                FD_ZERO(&cfds);
                FD_SET(client_fd, &cfds);
                if (select(client_fd + 1, &cfds, NULL, NULL, &cv) > 0) {
                    char probe;
                    if (recv(client_fd, &probe, 1, MSG_PEEK) == 0) {
                        killed = 1; /* client went away: nothing to send to */
                        break;
                    }
                }
            }
        }
        output[total] = '\0';
        close(out_pipe[0]);

        if (killed) {
            kill(pid, SIGKILL);
        }
        /* SIGCHLD is SIG_IGN process-wide (main.c): a child that already
         * exited was auto-reaped by the kernel and waitpid() returns ECHILD
         * with `status` untouched (zeroed) — which reads as "exited 0".
         * Only reject the output when we positively reaped the child and
         * saw a failure; a lost status means we trust the fully-read
         * output (same guard discipline as agent.c / router.c). */
        pid_t reaped;
        do {
            reaped = waitpid(pid, &status, 0);
        } while (reaped < 0 && errno == EINTR);
        int cgi_failed = (reaped == pid)
                             ? !(WIFEXITED(status) && WEXITSTATUS(status) == 0)
                             : 0;

        if (killed) {
            free(output);
            METRICS_INC(cgi_errors_total);
            /* reached only when killed != 0, so the status is always 504 */
            set_error_response(response, 504, "Gateway Timeout");
            return -1;
        }

        if (!cgi_failed) {
            char *body_start = strstr(output, "\r\n\r\n");
            if (body_start) {
                body_start += 4;
            } else {
                body_start = strstr(output, "\n\n");
                if (body_start) body_start += 2;
            }
            if (body_start) {
                size_t hdr_len = (size_t)(body_start - output);
                response->body_length = total - (int)hdr_len;

                /* CGI response headers (RFC 3875 section 6.2): Content-Type,
                 * plus Location (local or absolute) and Status. Location is
                 * also echoed to the client as a real Location header. */
                char *content_type = strstr(output, "Content-Type:");
                if (content_type && (size_t)(content_type - output) < hdr_len) {
                    content_type += 13;
                    while (*content_type == ' ') content_type++;
                    char *end = strchr(content_type, '\r');
                    if (!end) end = strchr(content_type, '\n');
                    if (end) {
                        int len = (int)(end - content_type);
                        if (len < (int)sizeof(response->content_type)) {
                            strncpy(response->content_type, content_type, len);
                            response->content_type[len] = '\0';
                        }
                    }
                } else {
                    strcpy(response->content_type, "text/html");
                }

                char *loc = strcasestr(output, "Location:");
                if (loc && (size_t)(loc - output) < hdr_len) {
                    loc += 9;
                    while (*loc == ' ') loc++;
                    char *end = strchr(loc, '\r');
                    if (!end) end = strchr(loc, '\n');
                    if (end) {
                        int len = (int)(end - loc);
                        if (len > 0 && len < (int)sizeof(response->location)) {
                            memcpy(response->location, loc, len);
                            response->location[len] = '\0';
                            if (strncmp(response->location, "/", 1) == 0) {
                                response->status_code = 302;
                                strcpy(response->status_text, "Found");
                            } else if (strstr(response->location, "://")) {
                                response->status_code = 302;
                                strcpy(response->status_text, "Found");
                            }
                        }
                    }
                }

                char *st_hdr = strcasestr(output, "Status:");
                if (st_hdr && (size_t)(st_hdr - output) < hdr_len) {
                    st_hdr += 7;
                    while (*st_hdr == ' ') st_hdr++;
                    int sc = atoi(st_hdr);
                    if (sc >= 100 && sc <= 599) {
                        response->status_code = sc;
                        const char *sp = strchr(st_hdr, ' ');
                        if (sp && *++sp) {
                            strncpy(response->status_text, sp,
                                    sizeof(response->status_text) - 1);
                            response->status_text[sizeof(response->status_text) - 1] = '\0';
                            char *eol = strchr(response->status_text, '\r');
                            if (!eol) eol = strchr(response->status_text, '\n');
                            if (eol) *eol = '\0';
                        }
                    }
                }

                /* Cache policy for script output. A CGI body is produced per
                 * request and often echoes the request back, so reusing one
                 * without asking us is never right. A script that knows its
                 * output better can still say so itself: an explicit
                 * Cache-Control wins, otherwise default to no-store.
                 * Silence is not neutral - with no policy at all a browser
                 * invents a heuristic freshness window and keeps serving the
                 * previous request's page. */
                char *cc = strcasestr(output, "Cache-Control:");
                if (cc && (size_t)(cc - output) < hdr_len) {
                    cc += 14;
                    while (*cc == ' ') cc++;
                    char *end = strchr(cc, '\r');
                    if (!end) end = strchr(cc, '\n');
                    if (end) {
                        int len = (int)(end - cc);
                        if (len > 0 && len < (int)sizeof(response->cache_control)) {
                            memcpy(response->cache_control, cc, (size_t)len);
                            response->cache_control[len] = '\0';
                        }
                    }
                } else {
                    set_str(response->cache_control, sizeof(response->cache_control), "no-store");
                }

                /* plain 200 only when Location/Status did not decide it */
                if (response->status_code == 0) {
                    response->status_code = 200;
                    strcpy(response->status_text, "OK");
                }
                if (response->body_length > get_cgi_body_tmp_threshold()) {
                    /* Big CGI output: spool to a temp file and stream it,
                     * same as large static files (no size ceiling). */
                    char tmp_name[MAX_PATH_SIZE];
                    snprintf(tmp_name, sizeof(tmp_name), "%s/cgi-out.XXXXXX", CGI_TMP_DIR);
                    int tmp_fd = mkstemp(tmp_name);
                    if (tmp_fd >= 0) {
                        FILE *tf = fdopen(tmp_fd, "wb");
                        if (tf) {
                            fwrite(body_start, 1, (size_t)response->body_length, tf);
                            fclose(tf);
                            response->stream_path = strdup(tmp_name);
                            response->stream_is_temp = (response->stream_path != NULL);
                        } else {
                            close(tmp_fd);
                            unlink(tmp_name);
                        }
                    }
                    if (!response->stream_path) {
                        free(output);
                        set_error_response(response, 507, "Insufficient Storage");
                        return -1;
                    }
                } else {
                    response->body = strdup(body_start);
                    if (!response->body) response->body_length = 0;
                }
            } else {
                free(output);
                response->status_code = 500;
                strcpy(response->status_text, "Internal Server Error");
                return -1;
            }
        } else {
            free(output);
            response->status_code = 500;
            strcpy(response->status_text, "Internal Server Error");
            return -1;
        }

        free(output);
    }

    return 0;
}
