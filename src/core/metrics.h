#ifndef METRICS_H
#define METRICS_H

/* Prometheus-style counters, shared across master + workers.
 * Include ordering: httpd.h consumers include this after the standard
 * headers; the struct below is self-contained. */
#include <stddef.h>

typedef struct {
    /* requests_total[0] = fast path (master loop), [1] = slow path. */
    unsigned long long requests_total[2];
    /* responses_total[0..4] = 1xx..5xx classes. */
    unsigned long long responses_total[5];
    /* Pool workers currently holding a connection (gauge). */
    unsigned long long workers_busy;
    /* Agent concurrency slot gauges; free mirrors the token pipe. */
    unsigned long long agent_slots_taken;
    unsigned long long agent_slots_free;
    /* Transient upstream failures that entered the retry loop. */
    unsigned long long chat_upstream_retries_total;
    /* CGI children killed by timeout or client disconnect. */
    unsigned long long cgi_errors_total;
} MetricsCounters;

extern MetricsCounters *g_metrics;

/* Create the shared table. Must run once in the parent before any worker
 * is forked (main.c init order). No-op failure keeps the server running
 * unmetered; increments become free no-ops (NULL guard). */
void metrics_init(void);
int metrics_up(void);

/* Render the current snapshot in Prometheus text format into buf.
 * Returns bytes written; always >= 0. bufsize >= 4096 is safe. */
int metrics_render(char *buf, int bufsize);

/* Lock-free increments. All of these tolerate g_metrics == NULL so call
 * sites never need a branch. */
#define METRICS_INC(field) do { \
    if (g_metrics) __atomic_fetch_add(&g_metrics->field, 1ULL, \
                                      __ATOMIC_RELAXED); \
} while (0)

#define METRICS_INC_AT(field, idx) do { \
    if (g_metrics && (idx) >= 0 && (idx) < (int)(sizeof g_metrics->field / \
                                                 sizeof g_metrics->field[0])) \
        __atomic_fetch_add(&g_metrics->field[idx], 1ULL, __ATOMIC_RELAXED); \
} while (0)

#define METRICS_SET(field, val) do { \
    if (g_metrics) __atomic_store_n(&g_metrics->field, \
                                    (unsigned long long)(val), \
                                    __ATOMIC_RELAXED); \
} while (0)

#endif
