/* CORS allowlist predicate — the single source of truth for "may this Origin
 * carry credentials on a cross-origin request".
 *
 * Why this is its own translation unit: it is consumed by two places with very
 * different link footprints —
 *   - src/http/http.c         (generic response headers via apply_cors_headers)
 *   - src/core/util.c         (framework same-origin gate, request_is_same_origin)
 * and both are exercised by unit tests that link a single .c file each. Keeping
 * the predicate with http.c made those tests fail to link (undefined
 * _cors_origin_allowed), and stubbing it in tests would let the stub drift away
 * from the real allowlist semantics.
 *
 * AGENT_CORS_ORIGINS is a comma-separated exact-match allowlist; unset disables
 * the layer (generic server default, i.e. plain same-origin only). */

#include <stdlib.h>
#include <string.h>

#include "internal.h"

int cors_origin_allowed(const char *origin) {
    const char *list = getenv("AGENT_CORS_ORIGINS");
    if (list == NULL || list[0] == '\0' || origin[0] == '\0') return 0;
    size_t olen = strlen(origin);
    const char *p = list;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        while (len > 0 && (p[0] == ' ' || p[0] == '\t')) { p++; len--; }
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' ||
                           p[len - 1] == '\r' || p[len - 1] == '\n')) len--;
        if (len == olen && strncmp(p, origin, len) == 0) return 1;
        if (comma == NULL) break;
        p = comma + 1;
    }
    return 0;
}
