/* ---- Per-IP rate limiting (-l <rps>, RATE_LIMIT_RPS) ----
 * Fixed-window counters in POSIX shared memory so every process (fork per
 * connection AND prefork workers) enforces the same limit. One bucket per
 * IP hashed into a fixed table; each bucket carries a tiny mutex. When
 * the per-second quota is exhausted the server answers 429 + Retry-After
 * and closes, before auth/dispatch - so brute force and CGI floods are
 * cheap to absorb. 0 disables. */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>

#include "internal.h"

#define RATE_TABLE_SIZE 1024

int g_rate_limit_rps = 0; /* disabled by default */

struct rate_bucket {
    pthread_mutex_t lock;
    unsigned int ip;
    int window; /* time(NULL) of the current fixed window */
    int count;
    int used;
};

static struct rate_bucket *g_rate_table = NULL; /* mmap(MAP_SHARED|MAP_ANON) */

void rate_limit_init(void) {
    if (g_rate_limit_rps <= 0) return;
    g_rate_table = mmap(NULL, RATE_TABLE_SIZE * sizeof(struct rate_bucket),
                        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (g_rate_table == MAP_FAILED) {
        perror("mmap rate table");
        g_rate_table = NULL;
        return;
    }
    memset(g_rate_table, 0, RATE_TABLE_SIZE * sizeof(struct rate_bucket));
    /* The table lives in MAP_SHARED memory and is used by forked children:
     * the mutexes must be process-shared or locking is undefined behavior. */
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    for (int i = 0; i < RATE_TABLE_SIZE; i++) {
        pthread_mutex_init(&g_rate_table[i].lock, &mattr);
    }
    pthread_mutexattr_destroy(&mattr);
}

/* Returns 1 when allowed, 0 when this IP exhausted its quota this second. */
int rate_limit_allow(unsigned int ip) {
    if (g_rate_limit_rps <= 0 || !g_rate_table) return 1;
    struct rate_bucket *b = &g_rate_table[ip % RATE_TABLE_SIZE];
    pthread_mutex_lock(&b->lock);
    /* Steal-the-bucket on collision keeps the table tiny without chaining:
     * a wrong match briefly credits/debits the wrong IP, acceptable for a
     * teaching server; the alternative (chaining) is not worth the memory. */
    if (!b->used || b->ip != ip) {
        b->ip = ip;
        b->window = (int)time(NULL);
        b->count = 0;
        b->used = 1;
    }
    int now = (int)time(NULL);
    if (b->window != now) {
        b->window = now;
        b->count = 0;
    }
    int allow = (b->count < g_rate_limit_rps);
    if (allow) b->count++;
    pthread_mutex_unlock(&b->lock);
    return allow;
}
