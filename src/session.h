#ifndef SESSION_H
#define SESSION_H

#include "minijson.h"

/* Memory (session persistence + fact store), port of resolve-studio's
 * session-store / usage persistence, C edition.
 *
 * Each conversation can carry a sessionId. State is kept as one JSON file
 * under .data/sessions/<id>.json (created on demand — including the
 * directory), written atomically (tmp file + rename) so forked workers
 * never expose torn state:
 *   { "id": "...",
 *     "messages": [ {"role":"user|assistant","content":"..."}, ... ],
 *     "facts":    { "<key>": "<value>", ... } }
 *
 * Two kinds of memory ride the chat loop:
 *   - transcript: the last user/assistant exchanges, replayed into the LLM
 *     history on the next request for the same sessionId;
 *   - facts: explicit key/value notes written by the model via the
 *     remember tool (opposite: recall). Both are re-injected into the
 *     system prompt on every run, so memory survives process restarts.
 *
 * Without a sessionId, facts fall back to a global .data/memory.json
 * (acts as the harness's long-term memory pool).
 */

#define SESSION_ID_MAX 64
#define SESSION_MSG_MAX 10
#define SESSION_FACTS_MAX 24
#define SESSION_KEY_MAX 64
#define SESSION_VAL_MAX 768
#define SESSION_CONTENT_MAX 2000

typedef struct {
    char role[16]; /* "user" | "assistant" */
    char content[SESSION_CONTENT_MAX + 1];
} SessionMsg;

typedef struct {
    char id[SESSION_ID_MAX + 1];
    int n_msgs;
    SessionMsg msgs[SESSION_MSG_MAX];
    int n_facts;
    struct {
        char key[SESSION_KEY_MAX + 1];
        char val[SESSION_VAL_MAX + 1];
    } facts[SESSION_FACTS_MAX];
} Session;

/* Generate a fresh session id (time + pid + RNG mixed, lowercase hex). */
void session_id_new(char *out, size_t sz);

/* True for path-safe ids within SESSION_ID_MAX: allowed bytes are alnum,
 * '-', '_', '+' (UI mints "s-<random>", tests use ids like "s1"). Anything
 * carrying '/', '\', '.', control bytes or other separators is rejected:
 * ids arrive in request bodies and must never feed a path (../../ escape). */
int session_id_valid(const char *id);

/* Load .data/sessions/<id>.json (or .data/memory.json for id==""). Returns
 * 0 when the file exists and parses, 1 when absent (empty session), or -1
 * on a hard error. Never truncates caller state. */
int session_load(const char *id, Session *s);

/* Atomically persist the session. Returns 0 on success. */
int session_save(const Session *s);

/* Startup hygiene: delete session files older than max_age_days (global
 * memory.json and files not matching the session-id shape are left alone). */
void session_prune_old(double max_age_days);

/* Append one transcript entry, dropping the oldest above SESSION_MSG_MAX. */
void session_append(Session *s, const char *role, const char *content);

/* Fact-store helpers (kept in memory; call session_save to persist). */
int session_fact_set(Session *s, const char *key, const char *val);
const char *session_fact_get(const Session *s, const char *key);

/* Persist a fact straight to disk without threading a Session through the
 * tool layer (the remember/recall tools only know the id): load, mutate,
 * save. -1 on failure (after saving the value passed). */
int session_fact_set_file(const char *id, const char *key, const char *val);
int session_fact_get_file(const char *id, const char *key,
                          char *out, size_t outsz);

/* Render the memory block for the system prompt: facts + recent transcript
 * (appended to `out`). */
void session_render_extra(const Session *s, sbuf *out);

#endif /* SESSION_H */