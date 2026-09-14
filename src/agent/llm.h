#ifndef LLM_H
#define LLM_H

#include "httpd.h"

/* Native C chat endpoint (POST/GET /api/chat): proxies an OpenAI-compatible
 * streaming upstream over HTTPS and re-emits the same SSE envelope the Node
 * backend uses (note/delta/error/done events). Configuration comes from the
 * environment (LLM_API_URL / LLM_API_KEY / LLM_MODEL, LLM_TIMEOUT seconds);
 * an empty LLM_API_KEY selects the built-in demo engine.
 *
 * Intercepted in handle_client (before the /react FastCGI relay and the
 * static/CGI dispatch), so it inherits the server's rate-limit and auth
 * gates. The handler streams the whole response (head + SSE body) itself
 * and sets response->handled; handle_client then only logs + closes. */
int llm_handle_chat(const HttpRequest *request, HttpResponse *response,
                    int client_fd);

/* Route predicate: /react/api/chat (exactly, optional query string) with
 * POST or GET. Checked in handle_client before the /react FastCGI relay. */
int llm_is_chat_route(const char *path, const char *method);

/* Route predicate: /react/api/pse (exactly, optional query string) with
 * POST only. Mirrors llm_is_chat_route but dispatches to the PSE
 * (Planner/Specialist/Evaluator) orchestrator regardless of PSE_ENABLED. */
int llm_is_pse_route(const char *path, const char *method);

/* Load LLM_* / AGENT_* from .env into the environment (once; existing env
 * wins). Normally lazy on first chat request; startup consumers like the
 * llm-router sync call it explicitly. */
void llm_env_init(void);

#endif /* LLM_H */
