#ifndef AGENT_H
#define AGENT_H

#include "httpd.h"
#include "minijson.h"
#include "chatio.h"

/* Agentic chat loop (ReAct: reason -> act -> observe, OpenAI tool-calling
 * dialect), src/agent.c. llm.c parses the request and dispatches here when
 * LLM_API_KEY is configured; everything that talks to the model lives in
 * this module, built on the same primitives as the CGI chain (fork curl
 * per round, pipes, poll timeouts). PSE (src/pse.c) drives the same
 * primitives: agent_round() for its one-shot Planner/Evaluator phases and
 * agent_run() (single "Specialist" loop) for the execution phase.
 *
 *   round: build messages (system + history + user [+ assistant/tool])
 *          -> fork curl (stream:true, tools:[...]) -> parse SSE:
 *              content deltas           -> relayed as "delta" events
 *              delta.tool_calls shards  -> accumulated per index
 *              finish_reason            -> "stop" ends the loop
 *          -> no tool calls? done. Otherwise execute each call via
 *             tools_dispatch, emit a "note" trace, append assistant +
 *             role:"tool" messages, next round.
 *
 * Concurrency guard: an agent run can hold its connection (and, in worker
 * mode, a worker) for minutes, so agent_run checks out one token from a
 * pipe-semaphore (AGENT_MAX_CONCURRENT, created once by agent_init in the
 * parent before workers fork) and fails fast with a "busy" error event
 * when none are free. PSE phases count as one run (single slot for the
 * whole orchestrator). */

#define AGENT_MAX_ROUNDS_DEFAULT 8
#define AGENT_MAX_CONCURRENT_DEFAULT 4
#define AGENT_TOOL_CALLS_MAX 8

/* Upstream error taxonomy (agent_round's up_err out-param). ZERO means a
 * clean round. Negative values are non-curl local failures; positive values
 * are curl(1) exit codes. Only the transient classes (timeout + connection
 * dropout codes) are retried — a rejected request (22) or missing curl
 * (127) would fail identically on the next attempt. */
#define UP_ERR_NONE 0
#define UP_ERR_LOCAL -1    /* pipe/fork/OOM inside agent_round (already emitted) */
#define UP_ERR_TIMEOUT -2  /* LLM_TIMEOUT expired while reading the stream */
#define UP_ERR_EMPTY -3    /* clean HTTP 200 but zero content and zero tool calls */
/* The upstream answered with a JSON error object (quota exhausted, bad
 * request...). That is a DEFINITE rejection, not a transient drop: it is
 * surfaced by handle_upstream_line as it arrives and must never be retried,
 * or one failure becomes three identical error events on the wire. */
#define UP_ERR_UPSTREAM -4
/* up_err >= UP_ERR_HTTP_BASE carries the upstream HTTP status as
 * up_err - UP_ERR_HTTP_BASE (captured via curl -w). Separates a HARD 4xx
 * reject (waste of a retry) from a 5xx/429 gateway failure (transient —
 * the llm-router's failover pool was momentarily exhausted, a retry can
 * land on a healthy provider). */
#define UP_ERR_HTTP_BASE 1000
#define AGENT_UPSTREAM_ATTEMPTS 3 /* total tries per round (retry transient drops) */
#define AGENT_BACKOFF_MS_1 1000   /* wait before 2nd try: restarting router etc. */
#define AGENT_BACKOFF_MS_2 2500   /* wait before 3rd try */

/* One parsed chat request (from llm.c). */
#define AGENT_MAX_MESSAGE 2000
#define AGENT_MAX_HISTORY 12
#define AGENT_HISTORY_MAX_CHARS 2000
#define AGENT_SESSION_ID_MAX 64

typedef struct {
    char message[AGENT_MAX_MESSAGE + 1];
    int n_history;
    char h_role[AGENT_MAX_HISTORY][16];
    char h_content[AGENT_MAX_HISTORY][AGENT_HISTORY_MAX_CHARS + 1];
    char session_id[AGENT_SESSION_ID_MAX + 1]; /* "" = no session */
} ChatRequest;

/* One tool_call accumulated from the streaming shards (raw argument TEXT is
 * kept unparsed — tools parse what they need). */
typedef struct {
    sbuf id;
    sbuf name;
    sbuf args;
} RoundCall;

typedef struct {
    RoundCall calls[AGENT_TOOL_CALLS_MAX];
    int n_calls;
    int saw_content; /* round streamed at least one content delta */
    int saw_error;   /* round carried a JSON "error" object (already surfaced) */
} RoundState;

/* Create the token pipe; call once in the PARENT before any worker/child
 * is forked so every process inherits the semaphore. Never fails hard —
 * on pipe failure the server runs without the concurrency cap. */
void agent_init(void);

/* Concurrency-slot primitives. agent_run checks out/releases one slot
 * itself; the PSE orchestrator checks one out for the whole run and drives
 * the bare phases (agent_round / agent_run_ex) underneath. */
int agent_slot_take(void);
void agent_slot_give(void);

/* Run one full agent conversation and stream it via out. system_extra is
 * appended to the default system prompt (skills index etc. — empty string
 * when unused). Takes one concurrency slot for the whole run. */
void agent_run(ChatOut *out, const ChatRequest *req, const char *system_extra);

/* Bare ReAct loop with an explicit base system prompt and an optional
 * capture buffer that collects the loop's streamed content (PSE's
 * Specialist phase passes its result buffer here; the Planner/Evaluator
 * phases use agent_round's own capture). Does NOT take a concurrency slot —
 * the PSE orchestrator holds one slot for all phases. Passing NULL for
 * system_base uses the built-in default. */
void agent_run_ex(ChatOut *out, const ChatRequest *req,
                  const char *system_base, const char *system_extra,
                  sbuf *capture);

/* One upstream chat/completions round (streams deltas as "delta" events).
 *   messages     raw JSON array of messages
 *   tools_json   JSON tools array, or "" to send none
 *   tool_choice  "none" to force tool_choice none, or NULL
 *   capture      optional sbuf that collects the streamed content text
 *                (final-answer capture for PSE phases)
 *   rs           receives any tool_calls shards; finish_reason out-param is
 *                one of "stop"/"tool_calls"/"other"/NULL.
 *   up_err       (out) UP_ERR_* or a curl exit code, so the caller can
 *                distinguish transient upstream drops from hard failures.
 * Returns 0 on a clean round, -1 on failure. For transient upstream errors
 * (timeout / connection dropout) NO error event is emitted — the caller
 * retries the round and only reports once tries are exhausted. */
int agent_round(ChatOut *out, const sbuf *messages, const char *tools_json,
                const char *tool_choice, sbuf *capture, RoundState *rs,
                const char **finish_reason, int *up_err);

/* Emit the user-facing error event for an agent_round failure code. No-op
 * for UP_ERR_LOCAL (that class is emitted inside agent_round as it happens)
 * and when the client already went away. Call once, after retries. */
void agent_emit_upstream_error(ChatOut *out, int up_err);

/* Free a RoundState returned by agent_round. */
void round_free(RoundState *rs);

/* The default (built-in) base system prompt. Shared by the single-loop
 * agent_run and llm.c assembly; PSE replaces it with an SOUL.md prompt. */
const char *agent_system_default(void);

/* Config (env-overridable, read per run): */
int agent_max_rounds(void);       /* AGENT_MAX_ROUNDS, default 8 */
int agent_max_concurrent(void);   /* AGENT_MAX_CONCURRENT, default 4 */
const char *agent_tool_source(void); /* AGENT_TOOL_SOURCE: "local" (default)
                                        or "gateway" (方案 A: tsm-hub runs
                                        the tool loop server-side) */

#endif /* AGENT_H */