/* Native C LLM chat endpoint — POST/GET /react/api/chat.
 *
 * This module is the endpoint shell: route predicate, request parsing
 * ({message, history?}), the offline demo engine and the LLM_*
 * .env
 * configuration (see also AGENT_* tuning in agent.h). The heavy lifting lives in sibling modules:
 *   - src/agent.c  ReAct loop + tool_calls streaming + fork-curl upstream
 *   - src/tools.c  built-in tool registry
 *   - src/chatio.c SSE transport primitives
 *   - src/minijson.c  sbuf + tolerant JSON reader
 * mirroring the old react-ssr/server/chat.ts contract (which remains the
 * make-dev/HMR implementation):
 *
 *   1. LLM_API_KEY set  -> agent_run() drives the multi-round tool loop.
 *      TLS stays curl's problem: the server itself still links nothing
 *      beyond libc (project rule: no OpenSSL).
 *   2. no key           -> local demo engine pacing a canned reply
 *      word-by-word, so the streaming path works offline with no
 *      credentials. This is a teaching server; the demo must not require
 *      a key.
 *
 * SSE envelope (identical to the node backend's, so the page is unchanged):
 *   data: {"t":"delta","d":"<text>"}\n\n   incremental tokens
 *   data: {"t":"note","d":"<text>"}\n\n    one-off status line (rounds and
 *                                          tool traces ride this)
 *   data: {"t":"error","d":"<text>"}\n\n   fatal, stream ends after this
 *   data: {"t":"done"}\n\n                 terminal marker
 *
 * The handler streams head + body itself and marks response->handled;
 * handle_client only logs (body_length doubles as the bytes-sent counter)
 * and closes — SSE is close-delimited, so no keep-alive. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>

#include "internal.h"
#include "llm.h"
#include "agent.h"
#include "pse.h"
#include "skills.h"
#include "session.h"

#define CHAT_DEFAULT_TIMEOUT 60

/* ------------------------------------------------------------------ */
/* Request body: {message, history?, sessionId?}                       */
/* ------------------------------------------------------------------ */

static int chat_parse_body(const char *body, ChatRequest *cr) {
    memset(cr, 0, sizeof *cr);
    if (!body) return -1;
    const char *p = jws(body);
    if (*p != '{') return -1;
    p++;
    for (;;) {
        p = jws(p);
        if (*p == '}') return 0;
        char key[32];
        if (!jread_string(&p, key, sizeof key)) return -1;
        p = jws(p);
        if (*p != ':') return -1;
        p = jws(p + 1);
        if (strcmp(key, "message") == 0) {
            if (!jread_string(&p, cr->message, sizeof cr->message)) return -1;
        } else if (strcmp(key, "history") == 0) {
            if (*p != '[') return -1;
            p++;
            for (;;) {
                p = jws(p);
                if (*p == ']') {
                    p++;
                    break;
                }
                if (*p == ',') {
                    p++;
                    continue;
                }
                if (*p != '{') return -1;
                p++;
                char role[16] = "";
                char content[AGENT_HISTORY_MAX_CHARS + 1] = "";
                int have_role = 0, have_content = 0;
                for (;;) {
                    p = jws(p);
                    if (*p == '}') {
                        p++;
                        break;
                    }
                    char hk[32];
                    if (!jread_string(&p, hk, sizeof hk)) return -1;
                    p = jws(p);
                    if (*p != ':') return -1;
                    p = jws(p + 1);
                    if (strcmp(hk, "role") == 0) {
                        if (!jread_string(&p, role, sizeof role)) return -1;
                        have_role = 1;
                    } else if (strcmp(hk, "content") == 0) {
                        if (!jread_string(&p, content, sizeof content)) return -1;
                        have_content = 1;
                    } else if (jskip_value(&p) != 0) {
                        return -1;
                    }
                    p = jws(p);
                    if (*p == ',') p++;
                }
                if (have_role && have_content &&
                    (strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0)) {
                    /* node parity: keep the LAST AGENT_MAX_HISTORY entries */
                    if (cr->n_history == AGENT_MAX_HISTORY) {
                        memmove(cr->h_role[0], cr->h_role[1],
                                (AGENT_MAX_HISTORY - 1) * sizeof cr->h_role[0]);
                        memmove(cr->h_content[0], cr->h_content[1],
                                (AGENT_MAX_HISTORY - 1) * sizeof cr->h_content[0]);
                        cr->n_history--;
                    }
                    strcpy(cr->h_role[cr->n_history], role);
                    strcpy(cr->h_content[cr->n_history], content);
                    cr->n_history++;
                }
                p = jws(p);
                if (*p == ',') p++;
            }
        } else if (jskip_value(&p) != 0) {
            return -1;
        }
        p = jws(p);
        if (*p == ',') {
            p++;
        } else if (*p == '}') {
            return 0;
        } else {
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Engine 2: local demo (no credentials needed)                        */
/* ------------------------------------------------------------------ */

static void chat_sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* One token = a word run or a whitespace run, so joins stay lossless
 * (node kept whitespace tokens for the same reason). */
static size_t chat_token_len(const char *p) {
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        return strspn(p, " \t\n\r");
    return strcspn(p, " \t\n\r");
}

static int starts_with_word(const char *m, const char *w) {
    size_t n = strlen(w);
    if (strncmp(m, w, n) != 0) return 0;
    return !isalpha((unsigned char)m[n]);
}

static int is_question(const char *m, const char *trimmed) {
    size_t n = strlen(trimmed);
    if (n > 0 && trimmed[n - 1] == '?') return 1;
    static const char *const stems[] = {
        "what", "why", "how", "when", "who", "where", "which",
        "can", "could", "does", "do", "is", "are", "tell", NULL,
    };
    for (int i = 0; stems[i]; i++) {
        if (starts_with_word(m, stems[i])) return 1;
    }
    return 0;
}

static void canned_reply(const char *message, char *buf, size_t bufsz) {
    char m[AGENT_MAX_MESSAGE + 1];
    size_t j = 0;
    for (size_t i = 0; message[i] && j < sizeof m - 1; i++) {
        m[j++] = (char)tolower((unsigned char)message[i]);
    }
    m[j] = '\0';

    char trimmed[AGENT_MAX_MESSAGE + 1];
    snprintf(trimmed, sizeof trimmed, "%s", message);
    trim_whitespace(trimmed);

    if (starts_with_word(m, "hi") || starts_with_word(m, "hello") ||
        starts_with_word(m, "hey") || starts_with_word(m, "yo")) {
        snprintf(buf, bufsz, "%s",
            "Hello! This reply is generated locally by agent-httpd's C demo engine — no external "
            "calls. It is streamed token-by-token over SSE straight from the server process, "
            "exactly like a real LLM reply would be. Ask me about the server, or set "
            "LLM_API_KEY to talk to a real model.");
        return;
    }
    if (strstr(m, "agent-httpd") || strstr(m, "server")) {
        snprintf(buf, bufsz, "%s",
            "agent-httpd is a ~3,000-line teaching HTTP server in C: keep-alive, a prefork worker "
            "pool with SCM_RIGHTS fd passing, FastCGI client+server, ETag/304, sendfile with "
            "Range/206, per-IP rate limiting and graceful drain. This chat page now rides the "
            "server's native SSE handler in src/llm.c — the model call is forked curl, the "
            "envelope is written by C.");
        return;
    }
    if (strstr(m, "stream") || strstr(m, "sse")) {
        snprintf(buf, bufsz, "%s",
            "The stream works like this: the browser POSTs to /react/api/chat, the C server "
            "forks curl against the OpenAI-compatible endpoint, re-emits each upstream delta "
            "as a text/event-stream event, and the page reads them with fetch + ReadableStream. "
            "Tokens appear one by one because every delta is flushed the moment it arrives.");
        return;
    }
    if (is_question(m, trimmed)) {
        snprintf(buf, bufsz, "%s",
            "Good question — but I am the offline demo engine, so my answer is canned: I detect "
            "questions, echo a bit of structure, and pace the words to demonstrate streaming. "
            "Try asking about agent-httpd, streaming or SSE — those I know. For real answers, "
            "set LLM_API_KEY and LLM_MODEL.");
        return;
    }
    int words = 0;
    for (const char *w = trimmed; *w; w += chat_token_len(w)) {
        if (*w != ' ' && *w != '\t') words++;
    }
    char echo[96];
    snprintf(echo, sizeof echo, "%s%s", message,
             strlen(message) > 80 ? "..." : "");
    echo[80] = '\0';
    snprintf(buf, bufsz,
             "You said: \"%s\" — %d words received. As the demo engine I mostly mirror and pace; "
             "ask about agent-httpd, streaming or SSE, or plug in a real model via LLM_API_KEY.",
             echo, words);
}

static void demo_reply(ChatOut *out, const char *message) {
    sse_event(out, "note",
              "demo engine (set LLM_API_KEY + LLM_MODEL for a real model)");
    if (!out->ok) return;

    char reply[1400];
    canned_reply(message, reply, sizeof reply);

    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)(time(NULL) ^ (getpid() << 8)));
        seeded = 1;
    }

    const char *p = reply;
    while (*p && out->ok) {
        /* Emit 1-2 tokens per tick, drawing the step ONCE so slice size and
         * increment always agree (node had the same single-draw rule). */
        int step = (rand() % 5 < 2) ? 2 : 1;
        const char *start = p;
        for (int k = 0; k < step && *p; k++) {
            p += chat_token_len(p);
        }
        size_t span = (size_t)(p - start);
        char tok[1536];
        if (span >= sizeof tok) span = sizeof tok - 1;
        memcpy(tok, start, span);
        tok[span] = '\0';
        sse_event(out, "delta", tok);
        if (*p) chat_sleep_ms(28);
    }
}

/* ------------------------------------------------------------------ */
/* Configuration + entry points                                        */
/* ------------------------------------------------------------------ */

/* .env loader, same convention as the node side (make dev loads it with
 * --env-file-if-exists). Used for LLM_* / AGENT_* chat config plus the
 * llm-router sync's MCP_FS_ROOT; a variable already present in the
 * environment — even empty — wins, so tests can force the demo engine next
 * to a real .env. */
void llm_env_init(void) {
    static int loaded = 0;
    if (loaded) return;
    loaded = 1;
    FILE *f = fopen(".env", "r");
    if (!f) return;
    char linebuf[512];
    while (fgets(linebuf, sizeof linebuf, f)) {
        char *s = linebuf;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\0' || *s == '\n') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        trim_whitespace(s);
        trim_whitespace(val);
        size_t n = strlen(val);
        if (n >= 2 && ((val[0] == '"' && val[n - 1] == '"') ||
                       (val[0] == '\'' && val[n - 1] == '\''))) {
            val[n - 1] = '\0';
            val++;
        }
        setenv(s, val, 0);
    }
    fclose(f);
}

static const char *llm_env_or(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return (v && *v) ? v : dflt;
}

int llm_is_chat_route(const char *path, const char *method) {
    if (strncmp(path, "/react/api/chat", 16) != 0) return 0;
    if (path[16] != '\0' && path[16] != '?') return 0;
    return (strcmp(method, "POST") == 0 || strcmp(method, "GET") == 0);
}

int llm_handle_chat(const HttpRequest *request, HttpResponse *response,
                    int client_fd) {
    ChatOut out = { client_fd, 1, 0, 0, {0} };
    size_t head_len = strlen(CHAT_SSE_HEAD);
    if (net_write_all(client_fd, CHAT_SSE_HEAD, head_len) == 0) {
        out.bytes += (int)head_len;
    } else {
        out.ok = 0;
    }

    ChatRequest *cr = malloc(sizeof *cr);
    if (!cr) {
        sse_event(&out, "error", "out of memory");
    } else if (chat_parse_body(request->body, cr) != 0) {
        sse_event(&out, "error", "request body must be JSON: {message, history?}");
    } else {
        trim_whitespace(cr->message);
        if (!cr->message[0]) {
            sse_event(&out, "error", "empty message");
        } else {
            /* optional top-level sessionId: turns on Memory (transcript +
             * fact store) for this request */
            const char *sidv = jfind_value(request->body, "sessionId");
            if (sidv && *sidv == '"') {
                if (jread_string(&sidv, cr->session_id,
                                 sizeof cr->session_id)) {
                    /* ids must be server-minted hex tokens; anything else is
                     * path traversal on the sessions dir, fail-closed */
                    if (cr->session_id[0] &&
                        !session_id_valid(cr->session_id)) {
                        sse_event(&out, "error", "sessionId rejected");
                        cr->session_id[0] = '\0';
                    }
                } else {
                    sse_event(&out, "error",
                              "sessionId must be a JSON string");
                }
            }
            llm_env_init();
            const char *key = llm_env_or("LLM_API_KEY", "");
            if (key[0]) {
                Session sess;
                memset(&sess, 0, sizeof sess);
                if (cr->session_id[0]) {
                    session_load(cr->session_id, &sess);
                    /* replay the transcript when the client sent no
                     * explicit history (node restores sessions on refresh) */
                    if (cr->n_history == 0) {
                        for (int i = 0; i < sess.n_msgs &&
                                        cr->n_history < AGENT_MAX_HISTORY; i++) {
                            strcpy(cr->h_role[cr->n_history], sess.msgs[i].role);
                            strcpy(cr->h_content[cr->n_history],
                                   sess.msgs[i].content);
                            cr->n_history++;
                        }
                    }
                }
                /* skills index + session memory ride along as system_extra */
                sbuf extra = {0};
                const char *idx = skills_index_text();
                if (idx && idx[0]) {
                    sb_str(&extra, idx);
                    sb_str(&extra, "\n\n");
                }
                session_render_extra(&sess, &extra);
                const char *extra_s = extra.p ? extra.p : "";
                out.cap_on = cr->session_id[0] ? 1 : 0;
                if (pse_enabled()) {
                    pse_run(&out, cr, extra_s);
                } else {
                    agent_run(&out, cr, extra_s);
                }
                /* persist transcript so the next request sees this turn; reload first
                 * so facts written mid-run (remember tool) survive the
                 * atomic overwrite */
                if (cr->session_id[0] && out.ok && out.cap.p && out.cap.len) {
                    session_load(cr->session_id, &sess);
                    session_append(&sess, "user", cr->message);
                    session_append(&sess, "assistant", out.cap.p);
                    session_save(&sess);
                }
                free(extra.p);
            } else {
                demo_reply(&out, cr->message);
            }
        }
    }
    free(cr);

    if (out.ok) sse_event(&out, "done", NULL);

    /* handle_client's handled path logs status_code + body_length and
     * closes the connection (SSE is close-delimited). */
    response->status_code = 200;
    strcpy(response->status_text, "OK");
    strcpy(response->content_type, "text/event-stream");
    response->body_length = out.bytes;
    response->handled = 1;
    return 0;
}
