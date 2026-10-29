#ifndef TOOLS_API_H
#define TOOLS_API_H

#include "httpd.h"

/* HTTP surface over the MCP tool registry (see tools_api.c).
 *
 *   GET  /react/api/tools        list registered MCP tools
 *   POST /react/api/tools/call   {"name":..., "arguments":{...}} → run it
 *
 * Both must be dispatched BEFORE the /react SSR/FastCGI relay. */

/* True when (path, method) is one of the two supported endpoints. */
int tools_api_is_route(const char *path, const char *method);

/* True for any path under /react/api/tools — used to keep those paths out of
 * the SSR relay / SPA fallback. */
int tools_api_is_reserved(const char *path);

/* Handle a request already classified by tools_api_is_route(). Fills
 * response (non-streaming) and returns 1. */
int tools_api_handle(const HttpRequest *request, HttpResponse *response);

#endif /* TOOLS_API_H */
