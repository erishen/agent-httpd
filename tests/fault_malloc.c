/* LD_PRELOAD malloc fault injector for the request-body OOM regression
 * test (tests/test_oom_body.c).
 *
 * Fails exactly one allocation: the one whose size equals AH_OOM_BYTES.
 * Every other call falls through to the real allocator, so the server's
 * own startup / worker / keep-alive allocations are untouched and only the
 * request-body buffer (Content-Length + 1) is the target — that is the
 * allocation src/http/http.c checks for NULL and answers 500 + close.
 *
 * Build (Linux CI / glibc):
 *   cc -shared -fPIC tests/fault_malloc.c -o tests/fault_malloc.so -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

static void *(*real_malloc)(size_t) = NULL;

static size_t oom_bytes(void) {
    const char *s = getenv("AH_OOM_BYTES");
    return s ? strtoull(s, NULL, 10) : 0;
}

void *malloc(size_t n) {
    if (!real_malloc)
        real_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    if (oom_bytes() != 0 && n == oom_bytes())
        return NULL;
    return real_malloc(n);
}
