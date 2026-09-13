/* Prometheus-style metrics for operations (see docs/ARCHITECTURE.md).
 *
 * Design constraints:
 *   - The counters live in MAP_SHARED anonymous memory created by
 *     metrics_init() in the parent BEFORE any worker is forked, so the
 *     master event loop and every pool worker increment the very same
 *     cells (same pattern as ratelimit.c).
 *   - Increments are __atomic_fetch_add on plain size_t cells: lock-free
 *     on the fast path (the master loop never takes a mutex to count a
 *     request), and exact enough for counters and gauges.
 *   - /metrics is served inline by process_request (like /health), so it
 *     rides the fast path in the event-loop mode and costs one send() in
 *     fork-per-connection mode. It sits behind the rate-limit and auth
 *     gates like every other route.
 *   - Scope is deliberately the handful of signals this architecture is
 *     interesting for: fast/slow split, status classes, worker saturation,
 *     agent slot pressure, upstream retry health, CGI failures, uptime.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#include "metrics.h"

MetricsCounters *g_metrics = NULL;
static time_t g_start_time;

void metrics_init(void) {
    if (g_metrics) return;
    g_metrics = mmap(NULL, sizeof(MetricsCounters),
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANON, -1, 0);
    if (g_metrics == MAP_FAILED) {
        g_metrics = NULL; /* counters stay off; the server runs unmetered */
        return;
    }
    memset(g_metrics, 0, sizeof(MetricsCounters));
    g_start_time = time(NULL);
}

int metrics_up(void) {
    return g_metrics != NULL;
}

/* Render the snapshot in Prometheus text exposition format (v0.0.4).
 * Returns the byte length written to buf (>= 0), or -1 when metrics are
 * disabled (mmap failed at init) — callers answer 200 with a note. */
int metrics_render(char *buf, int bufsize) {
    if (!g_metrics) {
        return snprintf(buf, bufsize,
                        "# metrics disabled (shared memory unavailable)\n");
    }
    static const char *const path_label[2] = {"fast", "slow"};
    static const char *const status_class[5] = {
        "1xx", "2xx", "3xx", "4xx", "5xx"};
    unsigned long long up = (unsigned long long)(time(NULL) - g_start_time);
    unsigned long long slots_max = g_metrics->agent_slots_taken +
                                   g_metrics->agent_slots_free;
    char *p = buf;
    char *end = buf + bufsize - 1;

#define EMIT(...) do { \
    if (p >= end) goto done; \
    p += snprintf(p, end - p + 1, __VA_ARGS__); \
} while (0)

    EMIT("# HELP agenthttpd_uptime_seconds Seconds since server start.\n");
    EMIT("# TYPE agenthttpd_uptime_seconds gauge\n");
    EMIT("agenthttpd_uptime_seconds %llu\n\n", up);

    EMIT("# HELP agenthttpd_requests_total Requests served, by dispatch path.\n");
    EMIT("# TYPE agenthttpd_requests_total counter\n");
    for (int i = 0; i < 2; i++) {
        EMIT("agenthttpd_requests_total{path=\"%s\"} %llu\n",
             path_label[i], g_metrics->requests_total[i]);
    }
    EMIT("\n");

    EMIT("# HELP agenthttpd_responses_total Responses by status class.\n");
    EMIT("# TYPE agenthttpd_responses_total counter\n");
    for (int i = 0; i < 5; i++) {
        EMIT("agenthttpd_responses_total{status=\"%s\"} %llu\n",
             status_class[i], g_metrics->responses_total[i]);
    }
    EMIT("\n");

    EMIT("# HELP agenthttpd_workers_busy Pool workers currently busy.\n");
    EMIT("# TYPE agenthttpd_workers_busy gauge\n");
    EMIT("agenthttpd_workers_busy %llu\n\n",
         g_metrics->workers_busy);

    EMIT("# HELP agenthttpd_agent_slots_taken Agent concurrency slots in use.\n");
    EMIT("# TYPE agenthttpd_agent_slots_taken gauge\n");
    if (slots_max > 0) {
        EMIT("agenthttpd_agent_slots_taken %llu\n",
             g_metrics->agent_slots_taken);
        EMIT("# TYPE agenthttpd_agent_slots_max gauge\n");
        EMIT("agenthttpd_agent_slots_max %llu\n\n", slots_max);
    } else {
        EMIT("agenthttpd_agent_slots_taken 0\n\n");
    }

    EMIT("# HELP agenthttpd_chat_upstream_retries_total Transient upstream failures retried.\n");
    EMIT("# TYPE agenthttpd_chat_upstream_retries_total counter\n");
    EMIT("agenthttpd_chat_upstream_retries_total %llu\n\n",
         g_metrics->chat_upstream_retries_total);

    EMIT("# HELP agenthttpd_cgi_errors_total CGI children killed (timeout/disconnect).\n");
    EMIT("# TYPE agenthttpd_cgi_errors_total counter\n");
    EMIT("agenthttpd_cgi_errors_total %llu\n",
         g_metrics->cgi_errors_total);

done:
    return (int)(p - buf);
#undef EMIT
}
