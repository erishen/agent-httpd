/* Agentic chat loop — see agent.h for the round shape and envelope.
 * Implementation notes:
 * - The upstream call is the llm.c fork-curl pattern moved here: one curl
 *   child per ROUND, request body piped via stdin, SSE parsed line-wise
 *   (any length) with the total-time budget from LLM_TIMEOUT.
 * - tool_calls arrive as fragmented deltas ("arguments" grows across
 *   chunks keyed by "index"); RoundCall accumulators are per-index sbufs
 *   and the raw argument TEXT is passed through unparsed — tools parse
 *   what they need.
 * - The messages array is kept as raw JSON text in an sbuf and re-wrapped
 *   into a fresh request body every round, so each round is an ordinary
 *   stateless /chat/completions call. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>

#include "internal.h"
#include "agent.h"
#include "metrics.h"
#include "tools.h"

#define AGENT_ARGS_MAX 32768        /* per tool_call arguments cap */
#define AGENT_TOOL_CALLS_MAX 8      /* per round */
#define AGENT_TOKEN_WAIT_MS 10000   /* busy timeout when all slots taken */
#define AGENT_LINE_MAX (1 << 16)    /* hard cap on one upstream SSE line */
#define SSE_HEARTBEAT_SECS 15        /* keep idle proxies (nginx :18081) alive */

/* ---- config ---------------------------------------------------------- */

int agent_max_rounds(void) {
    int v = env_int("AGENT_MAX_ROUNDS", AGENT_MAX_ROUNDS_DEFAULT);
    return v > 0 && v <= 30 ? v : AGENT_MAX_ROUNDS_DEFAULT;
}

int agent_max_concurrent(void) {
    int v = env_int("AGENT_MAX_CONCURRENT", AGENT_MAX_CONCURRENT_DEFAULT);
    return v > 0 && v <= 32 ? v : AGENT_MAX_CONCURRENT_DEFAULT;
}

/* Tool execution source.
 *   "local"  (default) agent-httpd runs its own ReAct loop and dispatches its
 *             built-in tools + spawned MCPs locally. The local tool schema is
 *             sent to tsm-hub; because a client tools array is present, tsm-hub
 *             passes the request through to the LLM and relays tool_calls back
 *             for local execution. This keeps EVERY local tool working — incl.
 *             the Beijing-time get_time fix and the domain MCPs (portfolio-check
 *             / weekly-investment) that tsm-hub does NOT have.
 *   "gateway" 方案 A draft: tsm-hub owns the capability pool and runs the tool
 *             loop server-side; agent-httpd sends NO local tool schema. CAVEAT
 *             (verified against tsm-hub internal/proxy): tsm-hub's proxy only
 *             runs its OWN server-side agent when the client sends NO tools
 *             array — otherwise it passes through. So in gateway mode tsm-hub
 *             executes its builtin get_time, which returns UTC (NOT Beijing)
 *             and it lacks the domain MCPs, i.e. this mode currently DOWNGRADES
 *             get_time to UTC and drops portfolio-check/weekly-investment. Do
 *             not enable until tsm-hub side is fixed (get_time +8 and domain
 *             MCPs adopted as external-mcps). */
const char *agent_tool_source(void) {
    const char *v = getenv("AGENT_TOOL_SOURCE");
    if (v && strcmp(v, "gateway") == 0) return "gateway";
    return "local";
}

/* ---- concurrency token pipe (created pre-fork in agent_init) --------- */

static int g_tok_r = -1;
static int g_tok_w = -1;

void agent_init(void) {
    int p[2];
    if (pipe(p) < 0) {
        perror("agent_init: pipe");
        return; /* runs uncapped */
    }
    g_tok_r = p[0];
    g_tok_w = p[1];
    int seeded = 0;
    for (int i = 0; i < agent_max_concurrent(); i++) {
        char tok = 1;
        ssize_t w = write(g_tok_w, &tok, 1);
        if (w == 1) seeded++;
    }
    /* Mirror the pipe fill level so /metrics can report slots_max. */
    if (g_metrics) {
        __atomic_store_n(&g_metrics->agent_slots_free,
                         (unsigned long long)seeded, __ATOMIC_RELAXED);
    }
}

/* Returns 1 when a slot was taken, 0 on timeout. */
int agent_slot_take(void) {
    if (g_tok_r < 0) return 1; /* cap not initialized: uncapped */
    struct pollfd pfd = { g_tok_r, POLLIN, 0 };
    if (poll(&pfd, 1, AGENT_TOKEN_WAIT_MS) <= 0) return 0;
    char tok;
    ssize_t r = read(g_tok_r, &tok, 1);
    if (r == 1) {
        if (g_metrics) __atomic_fetch_add(&g_metrics->agent_slots_taken,
                                         1ULL, __ATOMIC_RELAXED);
        if (g_metrics) __atomic_fetch_sub(&g_metrics->agent_slots_free,
                                         1ULL, __ATOMIC_RELAXED);
    }
    return r == 1;
}

void agent_slot_give(void) {
    if (g_tok_w < 0) return;
    char tok = 1;
    ssize_t w = write(g_tok_w, &tok, 1);
    if (w == 1) {
        if (g_metrics) __atomic_fetch_sub(&g_metrics->agent_slots_taken,
                                         1ULL, __ATOMIC_RELAXED);
        if (g_metrics) __atomic_fetch_add(&g_metrics->agent_slots_free,
                                         1ULL, __ATOMIC_RELAXED);
    }
}

/* ---- messages array (raw JSON text, reused across rounds) ------------ */

static void msgs_sep(sbuf *m) {
    if (m->len > 0) sb_chr(m, ',');
}

/* Opens the array with the system message; system_extra is appended into
 * the SAME content string ("\n\n" join) so the JSON stays well-formed. */
static void msgs_system(sbuf *m, const char *system_prompt,
                        const char *system_extra) {
    sbuf sys = {0};
    sb_str(&sys, system_prompt);
    if (system_extra && system_extra[0]) {
        sb_str(&sys, "\n\n");
        sb_str(&sys, system_extra);
    }
    sb_chr(m, '[');
    sb_str(m, "{\"role\":\"system\",\"content\":");
    sb_json_str(m, sys.p ? sys.p : system_prompt);
    sb_str(m, "}");
    free(sys.p);
}

static void msgs_role_text(sbuf *m, const char *role, const char *text) {
    msgs_sep(m);
    sb_str(m, "{\"role\":\"");
    sb_str(m, role);
    sb_str(m, "\",\"content\":");
    sb_json_str(m, text);
    sb_str(m, "}");
}

/* RoundCall / RoundState are declared in agent.h (PSE shares them). */

void round_free(RoundState *rs) {
    for (int i = 0; i < AGENT_TOOL_CALLS_MAX; i++) {
        free(rs->calls[i].id.p);
        free(rs->calls[i].name.p);
        free(rs->calls[i].args.p);
    }
    rs->n_calls = 0;
}

static RoundCall *round_call(RoundState *rs, long index) {
    if (index < 0 || index >= AGENT_TOOL_CALLS_MAX) return NULL;
    if (index + 1 > rs->n_calls) rs->n_calls = (int)index + 1;
    return &rs->calls[index];
}

/* Append the assistant tool_calls message + one role:"tool" message per
 * call (with its result text) to the ongoing messages array. */
static void msgs_append_round(sbuf *m, const RoundState *rs,
                              const sbuf *results) {
    msgs_sep(m);
    sb_str(m, "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[");
    for (int i = 0; i < rs->n_calls; i++) {
        if (i) sb_chr(m, ',');
        sb_str(m, "{\"id\":");
        sb_json_str(m, rs->calls[i].id.p ? rs->calls[i].id.p : "");
        sb_str(m, ",\"type\":\"function\",\"function\":{\"name\":");
        sb_json_str(m, rs->calls[i].name.p ? rs->calls[i].name.p : "?");
        sb_str(m, ",\"arguments\":");
        /* arguments is a JSON *string* holding the raw object text */
        sb_json_str(m, rs->calls[i].args.p ? rs->calls[i].args.p : "{}");
        sb_str(m, "}}");
    }
    sb_str(m, "]}");
    for (int i = 0; i < rs->n_calls; i++) {
        msgs_sep(m);
        sb_str(m, "{\"role\":\"tool\",\"tool_call_id\":");
        sb_json_str(m, rs->calls[i].id.p ? rs->calls[i].id.p : "");
        sb_str(m, ",\"content\":");
        sb_json_str(m, results[i].p && results[i].p[0]
                           ? results[i].p : "(no output)");
        sb_str(m, "}");
    }
}

/* ---- parsing one round's SSE line ------------------------------------ */

static void tc_process_element(const char *elem, RoundState *rs) {
    long index = 0;
    const char *iv = jfind_value(elem, "index");
    if (iv && isdigit((unsigned char)*iv)) index = strtol(iv, NULL, 10);
    RoundCall *rc = round_call(rs, index);
    if (!rc) return;

    const char *idv = jfind_value(elem, "id");
    if (idv && *idv == '"' && !rc->id.p) {
        char id[128];
        const char *ip = idv;
        if (jread_string(&ip, id, sizeof id)) sb_str(&rc->id, id);
    }
    const char *fn = jfind_value(elem, "function");
    if (!fn || *fn != '{') return;
    const char *nv = jfind_value(fn, "name");
    if (nv && *nv == '"' && !rc->name.p) {
        char nm[128];
        const char *np = nv;
        if (jread_string(&np, nm, sizeof nm)) sb_str(&rc->name, nm);
    }
    const char *av = jfind_value(fn, "arguments");
    if (av && *av == '"' && rc->args.len <= AGENT_ARGS_MAX) {
        /* arguments arrives in fragments; each is read into a bounded
         * scratch then appended to the per-index accumulator. The append
         * is clawed back to the remaining budget: a single oversized
         * fragment must not blow past AGENT_ARGS_MAX (the len guard above
         * is checked before the append, so it alone cannot bound it). */
        const char *ap = av;
        char *frag = malloc(strlen(av) + 1);
        if (frag && jread_string(&ap, frag, strlen(av) + 1)) {
            size_t room = AGENT_ARGS_MAX - rc->args.len;
            size_t fl = strlen(frag);
            if (fl > room) fl = room;
            if (fl > 0) sb_mem(&rc->args, frag, fl);
        }
        free(frag);
    }
}

/* Parse one SSE data line: content deltas relay live as "delta" events,
 * tool_call shards accumulate, finish_reason is captured. */
static void handle_upstream_line(ChatOut *out, char *line, RoundState *rs,
                                 int *content_started, int *done_seen,
                                 const char **finish_reason, sbuf *capture,
                                 int *http_status) {
    trim_whitespace(line);
    /* curl -w footer: a bare 3-digit HTTP status lands on its own final
     * line after the stream (only captured on the first occurrence). */
    if (http_status && *http_status < 0 && line[0] >= '0' && line[0] <= '9') {
        char *end = NULL;
        long st = strtol(line, &end, 10);
        if (end && *end == '\0' && st >= 100 && st < 1000) {
            *http_status = (int)st;
            return;
        }
    }
    const char *payload = NULL;
    if (strncmp(line, "data:", 5) == 0) {
        payload = jws(line + 5);
    } else {
        /* Some gateways emit errors as a bare JSON line with no SSE
         * "data:" prefix (observed on agnes rate limiting:
         * {"error":{"message":"You've reached the API rate limit..."}}).
         * Dropping the line silently turned the failure into an empty
         * "done" with zero deltas — surface it through the same error
         * path as data-framed errors. */
        payload = strchr(line, '{');
        if (payload == NULL || jfind_value(payload, "error") == NULL) return;
    }
    if (strcmp(payload, "[DONE]") == 0) {
        *done_seen = 1;
        return;
    }
    const char *ev = jfind_value(payload, "error");
    if (ev && strncmp(ev, "null", 4) != 0) {
        char msg[512] = "upstream sent an error event";
        const char *em = NULL;
        if (*ev == '"') {
            em = ev;
        } else {
            em = jfind_value(ev, "message");
            if (em && *em != '"') em = NULL;
        }
        if (em && jread_string(&em, msg, sizeof msg) == NULL) {
            strcpy(msg, "upstream sent an error event");
        }
        sse_event(out, "error", msg);
        return;
    }
    const char *choices = jfind_value(payload, "choices");
    if (!choices || *choices != '[') return;
    const char *obj = strchr(choices, '{');
    if (!obj) return;
    if (!*finish_reason) {
        const char *fr = jfind_value(obj, "finish_reason");
        if (fr && *fr == '"') {
            char fbuf[24];
            const char *fp = fr;
            if (jread_string(&fp, fbuf, sizeof fbuf)) {
                *finish_reason = strcmp(fbuf, "stop") == 0 ? "stop"
                               : strcmp(fbuf, "tool_calls") == 0 ? "tool_calls"
                               : "other";
            }
        }
    }
    const char *delta = jfind_value(obj, "delta");
    if (!delta || *delta != '{') return;

    /* content: relay as deltas (leading-gap trim on the round's first one) */
    const char *content = jfind_value(delta, "content");
    if (content && *content == '"') {
        char *text = malloc(strlen(line) + 1);
        if (text) {
            if (jread_string(&content, text, strlen(line) + 1)) {
                char *s = text;
                if (!*content_started) {
                    while (*s == '\n' || *s == '\r' ||
                           *s == ' ' || *s == '\t') s++;
                }
                if (*s) {
                    *content_started = 1;
                    sse_event(out, "delta", s);
                    if (capture) sb_str(capture, s);
                }
            }
            free(text);
        }
    }

    /* tool_calls shards */
    const char *tcs = jfind_value(delta, "tool_calls");
    if (!tcs || *tcs != '[') return;
    const char *q = jws(tcs + 1);
    while (*q && *q != ']') {
        if (*q != '{') break;
        tc_process_element(q, rs);
        if (jskip_value(&q) != 0) break;
        q = jws(q);
        if (*q == ',') {
            q = jws(q + 1);
        } else {
            break;
        }
    }
}

/* ---- one upstream round ---------------------------------------------- */

/* Append at most the remaining AGENT_LINE_MAX bytes of a line; a line that
 * overruns is flagged (and dropped) instead of growing the buffer forever. */
static void line_buf_append(sbuf *line, const char *cur, size_t n, int *ovf) {
    if (line->len >= AGENT_LINE_MAX) {
        *ovf = 1;
        return;
    }
    if (n > (size_t)AGENT_LINE_MAX - line->len) {
        n = (size_t)AGENT_LINE_MAX - line->len;
        *ovf = 1;
    }
    sb_mem(line, cur, n);
}

/* Fork one curl round against the upstream. Streams content deltas as
 * "delta" events, accumulates tool_call shards into rs, captures
 * finish_reason, and (when `capture` is non-NULL) mirrors every streamed
 * content delta into it — PSE's one-shot Planner/Evaluator phases use this
 * to collect their text while the page still sees token typing. Returns 0
 * on success, -1 on a fatal error (error event already emitted). */
int agent_round(ChatOut *out, const sbuf *messages, const char *tools_json,
                const char *tool_choice, sbuf *capture, RoundState *rs,
                const char **finish_reason, int *up_err) {
    const char *api_url = getenv("LLM_API_URL");
    const char *api_key = getenv("LLM_API_KEY");
    const char *model = getenv("LLM_MODEL");
    *finish_reason = NULL;
    if (up_err) *up_err = UP_ERR_NONE;
    int content_started = 0;

    sbuf body = {0};
    sb_str(&body, "{\"model\":");
    sb_json_str(&body, model ? model : "");
    sb_str(&body, ",\"stream\":true");
    if (tools_json && tools_json[0]) {
        sb_str(&body, ",\"tools\":");
        sb_str(&body, tools_json);
    }
    if (tool_choice && strcmp(tool_choice, "none") == 0) {
        sb_str(&body, ",\"tool_choice\":\"none\"");
    }
    sb_str(&body, ",\"messages\":");
    sb_mem(&body, messages->p ? messages->p : "[",
           messages->len ? messages->len : 1);
    /* the msgs buffer stays OPEN across rounds (msgs_append_round reuses
     * it), so the closing bracket is only materialized at send time */
    sb_chr(&body, ']');
    sb_str(&body, "}");
    if (body.oom) {
        if (up_err) *up_err = UP_ERR_LOCAL;
        sse_event(out, "error", "out of memory building upstream request");
        return -1;
    }

    char auth[1024];
    /* api_key may be unset in dev/test: a NULL here is UB for %s (and on
     * some libc a hard crash), so substitute an empty bearer instead of
     * letting the upstream call fail catastrophically. */
    snprintf(auth, sizeof auth, "Authorization: Bearer %s",
             api_key ? api_key : "");
    int timeout_s = env_int("LLM_TIMEOUT", 60);
    if (timeout_s <= 0 || timeout_s > 600) timeout_s = 60;
    char max_time[16];
    snprintf(max_time, sizeof max_time, "%d", timeout_s);

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        if (up_err) *up_err = UP_ERR_LOCAL;
        sse_event(out, "error", "pipe failed");
        free(body.p);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        if (up_err) *up_err = UP_ERR_LOCAL;
        sse_event(out, "error", "fork failed");
        free(body.p);
        return -1;
    }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        /* --http1.1: some CDN paths (e.g. Cloudflare fronts seen from certain
         * cloud egress IPs) complete the h2 TLS handshake then never deliver
         * a byte — h1.1 on the same route works. h2 buys nothing for an SSE
         * relay, so pin the protocol. */
        execlp("curl", "curl", "-sS", "--http1.1", "-N", "-f", "--max-time", max_time,
               "-w", "\n%{http_code}\n",
               "-X", "POST", "-H", auth,
               "-H", "Content-Type: application/json",
               "--data-binary", "@-", api_url, (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    /* EPIPE here just means curl died early — keep reading for its status */
    {
        size_t off = 0;
        while (off < body.len) {
            ssize_t w = write(in_pipe[1], body.p + off, body.len - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            off += (size_t)w;
        }
    }
    close(in_pipe[1]);

    time_t deadline = time(NULL) + timeout_s;
    sbuf line = {0};
    int done_seen = 0, eof = 0, timed_out = 0, stop = 0, http_status = -1;
    int line_ovf = 0;
    for (;;) {
        long remain = (long)(deadline - time(NULL));
        if (remain <= 0) {
            timed_out = 1;
            break;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(out_pipe[0], &rfds);
        struct timeval tv;
        /* Cap each select() at the heartbeat interval (unless the upstream
         * deadline is nearer) so a long think gap emits a comment instead of
         * letting a reverse proxy idle us out. */
        long wait = remain;
        if (wait > SSE_HEARTBEAT_SECS) wait = SSE_HEARTBEAT_SECS;
        tv.tv_sec = wait;
        tv.tv_usec = 0;
        int r = select(out_pipe[0] + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) {
            if (wait < remain) {
                /* heartbeat interval elapsed, not the real deadline: nudge the
                 * client so proxies (nginx :18081) reset their idle timer. */
                sse_heartbeat(out);
                if (!out->ok) break;  /* client already gone */
                continue;
            }
            timed_out = 1;
            break;
        }
        char chunk[4096];
        ssize_t n = read(out_pipe[0], chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) {
            eof = 1;
            break;
        }
        char *cur = chunk;
        ssize_t left = n;
        while (left > 0 && !stop) {
            char *nl = memchr(cur, '\n', (size_t)left);
            if (!nl) {
                line_buf_append(&line, cur, (size_t)left, &line_ovf);
                break;
            }
            line_buf_append(&line, cur, (size_t)(nl - cur), &line_ovf);
            if (line.oom) {
                sse_event(out, "error", "out of memory reading upstream");
                stop = 1;
                break;
            }
            if (line.len > 0 && !line_ovf) {
                handle_upstream_line(out, line.p, rs, &content_started,
                                     &done_seen, finish_reason, capture,
                                     &http_status);
            }
            line.len = 0;
            if (line.p) line.p[0] = '\0';
            line_ovf = 0;
            if (done_seen || !out->ok) stop = 1;
            left -= (nl - cur) + 1;
            cur = nl + 1;
        }
        if (stop) break;
    }
    if (!stop && out->ok && line.len > 0 && !line.oom && !line_ovf) {
        handle_upstream_line(out, line.p, rs, &content_started, &done_seen,
                             finish_reason, capture, &http_status);
    }
    free(line.p);
    close(out_pipe[0]);
    if (!eof) kill(pid, SIGKILL);
    /* SIGCHLD is SIG_IGN process-wide (main.c), so the kernel reaps curl the
     * moment it exits and this waitpid() comes back ECHILD with `status`
     * untouched. A zeroed status reads as "exited 0", which turned a fast
     * upstream failure (connection refused, bad host, immediate reset) into a
     * clean, empty answer: no error event, no retry, nothing in the log.
     * router.c already guards its own child this way; do the same here. */
    int status = 0;
    pid_t reaped;
    do {
        reaped = waitpid(pid, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    int code_known = (reaped == pid);

    int rc = 0;
    if (out->ok) {
        int code = (code_known && WIFEXITED(status)) ? WEXITSTATUS(status) : 0;
        if (timed_out) {
            if (up_err) *up_err = UP_ERR_TIMEOUT;
            rc = -1;
        } else if (!done_seen && (code != 0 || http_status < 0)) {
            /* code != 0: curl reported a failure. http_status < 0: the -w
             * footer never arrived, so no HTTP status was ever produced —
             * the transport failed even if the exit status got lost above
             * (curl 7 refused / 6 bad host). Both are round failures. */
            if (up_err) {
                /* curl -f maps every HTTP>=400 to exit 22; the -w footer
                 * carries the REAL status, which decides retry semantics. */
                if (http_status >= 400) *up_err = UP_ERR_HTTP_BASE + http_status;
                else if (code != 0) *up_err = code;
                else *up_err = 7; /* curl: couldn't connect */
            }
            rc = -1;
        }
    }
    free(body.p);
    return rc;
}

/* User-facing message for a failed round; the caller reports it once all
 * retry attempts are exhausted (or immediately for the hard classes). */
void agent_emit_upstream_error(ChatOut *out, int up_err) {
    if (up_err == UP_ERR_LOCAL || up_err == UP_ERR_NONE || !out->ok) return;
    char msg[192];
    if (up_err >= UP_ERR_HTTP_BASE) {
        int st = up_err - UP_ERR_HTTP_BASE;
        if (st >= 500) {
            snprintf(msg, sizeof msg,
                     "upstream unavailable (HTTP %d); the model pool was temporarily out of free quota", st);
        } else if (st == 429) {
            snprintf(msg, sizeof msg,
                     "upstream rate limited (HTTP 429); wait a moment and retry");
        } else {
            /* diagnosis hint goes to the server log only — remote users get
             * a plain rejection, not the env variable names to attack */
            fprintf(stderr, "[agent] upstream rejected: HTTP %d (check LLM_API_URL / LLM_API_KEY)\n", st);
            snprintf(msg, sizeof msg,
                     "upstream rejected the request (HTTP %d)", st);
        }
        sse_event(out, "error", msg);
        return;
    }
    switch (up_err) {
    case UP_ERR_TIMEOUT:
        snprintf(msg, sizeof msg, "upstream timeout");
        break;
    case 127:
        fprintf(stderr, "[agent] curl(1) not found in PATH\n");
        snprintf(msg, sizeof msg, "upstream engine unavailable on the server");
        break;
    case 22:
        fprintf(stderr, "[agent] upstream HTTP error (check LLM_API_URL / LLM_API_KEY)\n");
        snprintf(msg, sizeof msg, "upstream rejected the request");
        break;
    default:
        /* curl exit codes are infrastructure detail: log locally, keep the
         * wire message generic so nothing about the transport leaks */
        fprintf(stderr, "[agent] upstream failed (curl exit %d)\n", up_err);
        snprintf(msg, sizeof msg, "upstream connection failed; retry in a moment");
        break;
    }
    sse_event(out, "error", msg);
}

/* Transient = worth retrying with identical messages: timeouts, connection
 * dropout (curl 18 partial, 7/35/52/55/56 network reset...), and gateway
 * answers the router's failover pool was momentarily out of quota (5xx,
 * or a 429 that throttled the whole chain). Hard classes: 4xx rejects
 * (except 429 — upstream quota, will free up) and 127 (no curl). Local
 * failures were already surfaced inside agent_round. */
static int agent_retryable(int up_err) {
    if (up_err == UP_ERR_TIMEOUT) return 1;
    if (up_err >= UP_ERR_HTTP_BASE) {
        int st = up_err - UP_ERR_HTTP_BASE;
        return st == 429 || st >= 500;
    }
    return up_err > 0 && up_err != 22 && up_err != 127;
}

/* ---- the loop ---------------------------------------------------------- */

const char *agent_system_default(void) {
    return
        "You are the assistant embedded in agent-httpd, a small C web server that "
        "implements its chat agent natively (ReAct with tool calls). Use the "
        "provided tools when they help; answer concisely.";
}

static void agent_run_inner(ChatOut *out, const ChatRequest *req,
                            const char *system_base, const char *system_extra,
                            int max_rounds, sbuf *capture) {
    sbuf msgs = {0};
    int gateway_mode = (strcmp(agent_tool_source(), "gateway") == 0);
    msgs_system(&msgs, system_base, system_extra);
    for (int i = 0; i < req->n_history; i++) {
        msgs_role_text(&msgs, req->h_role[i], req->h_content[i]);
    }
    msgs_role_text(&msgs, "user", req->message);

    int rounds = max_rounds > 0 ? max_rounds : agent_max_rounds();
    for (int round = 1; round <= rounds && out->ok; round++) {
        if (round > 1) {
            char note[48];
            snprintf(note, sizeof note, "round %d/%d", round, rounds);
            sse_event(out, "note", note);
        }

        /* A provider can drop a stream mid-answer (curl exit 18 & friends).
         * Retry the round with identical messages — msgs only grows after a
         * SUCCESSFUL round, so a failed attempt never commits anything. */
        RoundState rs;
        const char *finish = NULL;
        int rc = 0, up_err = 0;
        for (int attempt = 1; attempt <= AGENT_UPSTREAM_ATTEMPTS; attempt++) {
            memset(&rs, 0, sizeof rs);
            up_err = 0;
            finish = NULL;
            rc = agent_round(out, &msgs,
                             gateway_mode ? NULL : tools_schema_json(),
                             NULL, capture, &rs, &finish, &up_err);
            if (rc == 0 || !agent_retryable(up_err) ||
                attempt == AGENT_UPSTREAM_ATTEMPTS)
                break;
            round_free(&rs);
            /* Backoff between tries: an upstream that is rolling-restarting
             * (llm-router restarts are the common case here) is typically
             * back within seconds, so an instant retry always loses. Sleep in
             * 100ms steps so a client disconnect (out->ok == 0) aborts the
             * wait instead of stalling the worker for nothing. */
            int backoff_ms = (attempt == 1) ? AGENT_BACKOFF_MS_1 : AGENT_BACKOFF_MS_2;
            METRICS_INC(chat_upstream_retries_total);
            if (out->ok) {
                char note[96];
                snprintf(note, sizeof note,
                         "upstream hiccup, retrying in %.1fs (attempt %d/%d)",
                         backoff_ms / 1000.0, attempt + 1, AGENT_UPSTREAM_ATTEMPTS);
                sse_event(out, "note", note);
            }
            for (int s = 0; s < backoff_ms && out->ok; s += 100) {
                usleep(100 * 1000);
            }
        }
        if (rc != 0) {
            if (out->ok) agent_emit_upstream_error(out, up_err);
            round_free(&rs);
            break;
        }
        /* no tool calls -> the final answer already streamed; done */
        if (!out->ok || rs.n_calls == 0) {
            round_free(&rs);
            break;
        }

        /* 方案 A (gateway): we sent no local tool schema, so any tool_calls
         * reaching us are unexpected pass-through we cannot execute. This only
         * happens if tsm-hub bypassed its own server-side agent. Surface and
         * stop instead of looping on an empty dispatch. NOTE: in gateway mode
         * get_time/domain tools are already lost upstream (see agent_tool_source
         * caveat) — this branch is a safety net, not a fix. */
        if (gateway_mode) {
            sse_event(out, "note", "tool step handled by upstream gateway");
            round_free(&rs);
            break;
        }

        /* synthesize ids for providers that omit them */
        for (int i = 0; i < rs.n_calls; i++) {
            if (!rs.calls[i].id.p) {
                char id[32];
                snprintf(id, sizeof id, "call_%d", i);
                sb_str(&rs.calls[i].id, id);
            }
        }

        /* announce, then execute */
        for (int i = 0; i < rs.n_calls && out->ok; i++) {
            sbuf trace = {0};
            sb_str(&trace, "tool ");
            sb_str(&trace, rs.calls[i].name.p ? rs.calls[i].name.p : "?");
            sb_str(&trace, "(");
            sb_str(&trace, rs.calls[i].args.p ? rs.calls[i].args.p : "");
            sb_str(&trace, ")");
            sse_event(out, "note", trace.p ? trace.p : "tool call");
            free(trace.p);
        }
        sbuf results[AGENT_TOOL_CALLS_MAX];
        memset(results, 0, sizeof results);
        for (int i = 0; i < rs.n_calls && out->ok; i++) {
            tools_dispatch(rs.calls[i].name.p ? rs.calls[i].name.p : "?",
                           rs.calls[i].args.p,
                           req->session_id[0] ? req->session_id : NULL,
                           &results[i]);
        }

        if (out->ok) {
            msgs_append_round(&msgs, &rs, results);
        }
        for (int i = 0; i < AGENT_TOOL_CALLS_MAX; i++) free(results[i].p);
        round_free(&rs);
        if (!out->ok) break;
    }
    free(msgs.p);
}

void agent_run_ex(ChatOut *out, const ChatRequest *req,
                  const char *system_base, const char *system_extra,
                  sbuf *capture) {
    agent_run_inner(out, req,
                    system_base ? system_base : agent_system_default(),
                    system_extra, agent_max_rounds(), capture);
}

void agent_run(ChatOut *out, const ChatRequest *req, const char *system_extra) {
    if (!agent_slot_take()) {
        sse_event(out, "error",
                  "server busy (AGENT_MAX_CONCURRENT reached), retry shortly");
        return;
    }
    agent_run_ex(out, req, NULL, system_extra, NULL);
    agent_slot_give();
}
