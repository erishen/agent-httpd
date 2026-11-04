/* Per-(user, ip) login failure limiter — brute-force / password-spray guard
 * for the /login form (handle_login, http_route.c).
 *
 * The per-IP token bucket (ratelimit.c) throttles request VOLUME; it cannot
 * tell a success from a failure, so a botnet or an attacker rotating IPs can
 * keep guessing one password without ever tripping it. This limiter counts
 * CONSECUTIVE FAILED logins per (username, client IP) and locks that pair
 * out with an exponentially backing-off timeout.
 *
 * Shared state lives in POSIX shared memory (mmap MAP_SHARED|MAP_ANON with
 * process-shared mutexes — the same pattern as ratelimit.c), so every
 * prefork worker enforces the same lockout. A success clears the pair; a
 * stale streak (no failure for LOGIN_RESET_SECONDS) restarts from one.
 *
 * Policy:
 *   LOGIN_MAX_FAILURES (5) consecutive failures lock (user, ip) for
 *   LOGIN_BACKOFF_BASE (30s); every further failure while locked extends the
 *   lockout exponentially, capped at LOGIN_MAX_LOCKOUT (15 min). The check
 *   runs BEFORE the password compare, so a lockout can never be ended by
 *   guessing the password mid-lockout.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>

#include "internal.h"

#define LOGIN_TABLE_SIZE 1024       /* (user,ip) pairs; small is fine */
#define LOGIN_MAX_PROBES 8
#define LOGIN_MAX_FAILURES 5        /* failures before the first lockout */
#define LOGIN_BACKOFF_BASE 30       /* seconds, first lockout */
#define LOGIN_MAX_LOCKOUT 900       /* 15 min cap on the backoff */
#define LOGIN_RESET_SECONDS 900     /* streak window before a reset */

struct login_bucket {
    pthread_mutex_t lock;
    unsigned int key;       /* FNV-1a(user + "\xff" + ip); 0 = free slot */
    int fail_count;         /* consecutive failures in the current streak */
    long long first_fail;   /* epoch of the first failure in the streak */
    long long lock_until;   /* epoch until which (user, ip) is refused */
};

static struct login_bucket *g_login_table = NULL; /* mmap(MAP_SHARED|MAP_ANON) */

/* FNV-1a 32-bit over user + separator + ip. Never returns 0 (0 means "free
 * slot" to the probe loop). */
static unsigned int login_key(const char *user, const char *ip) {
    unsigned int h = 0x811c9dc5u;
    const unsigned char *p = (const unsigned char *)user;
    while (*p) { h ^= *p++; h *= 0x01000193u; }
    h ^= 0xffu; h *= 0x01000193u;
    p = (const unsigned char *)ip;
    while (*p) { h ^= *p++; h *= 0x01000193u; }
    return h ? h : 1u;
}

void login_limiter_init(void) {
    if (g_login_table) return;
    g_login_table = mmap(NULL, LOGIN_TABLE_SIZE * sizeof(struct login_bucket),
                         PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (g_login_table == MAP_FAILED) {
        perror("mmap login limiter table");
        g_login_table = NULL; /* every limiter call then no-ops */
        return;
    }
    memset(g_login_table, 0, LOGIN_TABLE_SIZE * sizeof(struct login_bucket));
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    for (int i = 0; i < LOGIN_TABLE_SIZE; i++) {
        pthread_mutex_init(&g_login_table[i].lock, &mattr);
    }
    pthread_mutexattr_destroy(&mattr);
}

/* Seconds until the (user, ip) lockout expires; 0 = not locked. Also 0 when
 * the table is unavailable (degrade open rather than lock the whole site
 * because mmap failed at startup). */
long login_limiter_blocked_for(const char *user, const char *ip) {
    if (!g_login_table || !user || !ip) return 0;
    unsigned int key = login_key(user, ip);
    unsigned int base = key % LOGIN_TABLE_SIZE;
    long long now = (long long)time(NULL);
    for (int i = 0; i < LOGIN_MAX_PROBES; i++) {
        struct login_bucket *b = &g_login_table[(base + i) % LOGIN_TABLE_SIZE];
        pthread_mutex_lock(&b->lock);
        int hit = (b->key == key);
        long long until = hit ? b->lock_until : 0;
        pthread_mutex_unlock(&b->lock);
        if (hit || !b->key) {
            if (!hit) return 0;
            return until > now ? (long)(until - now) : 0;
        }
    }
    return 0; /* every probe slot busy: let it through */
}

/* Record one failed login for (user, ip): extend the streak, and once it
 * reaches LOGIN_MAX_FAILURES lock the pair out with exponential backoff. */
void login_limiter_fail(const char *user, const char *ip) {
    if (!g_login_table || !user || !ip) return;
    unsigned int key = login_key(user, ip);
    unsigned int base = key % LOGIN_TABLE_SIZE;
    long long now = (long long)time(NULL);
    for (int i = 0; i < LOGIN_MAX_PROBES; i++) {
        struct login_bucket *b = &g_login_table[(base + i) % LOGIN_TABLE_SIZE];
        pthread_mutex_lock(&b->lock);
        if (!b->key) {
            b->key = key;
            b->first_fail = now;
            b->fail_count = 1;
            pthread_mutex_unlock(&b->lock);
            return;
        }
        if (b->key == key) {
            if (now - b->first_fail > LOGIN_RESET_SECONDS) {
                b->first_fail = now;
                b->fail_count = 1;
            } else {
                b->fail_count++;
            }
            if (b->fail_count >= LOGIN_MAX_FAILURES) {
                long long backoff =
                    (long long)LOGIN_BACKOFF_BASE
                    << (b->fail_count - LOGIN_MAX_FAILURES);
                if (backoff > LOGIN_MAX_LOCKOUT) backoff = LOGIN_MAX_LOCKOUT;
                b->lock_until = now + backoff;
            }
            pthread_mutex_unlock(&b->lock);
            return;
        }
        pthread_mutex_unlock(&b->lock);
    }
    /* every probe slot busy: drop the failure rather than clobber another
     * pair's accounting (degrade) */
}

/* A successful login clears the pair: releases the slot so its counter and
 * lockout are gone (a correct password ends the streak, as it should). */
void login_limiter_success(const char *user, const char *ip) {
    if (!g_login_table || !user || !ip) return;
    unsigned int key = login_key(user, ip);
    unsigned int base = key % LOGIN_TABLE_SIZE;
    for (int i = 0; i < LOGIN_MAX_PROBES; i++) {
        struct login_bucket *b = &g_login_table[(base + i) % LOGIN_TABLE_SIZE];
        pthread_mutex_lock(&b->lock);
        if (b->key == key) {
            b->key = 0;
            b->fail_count = 0;
            b->lock_until = 0;
            pthread_mutex_unlock(&b->lock);
            return;
        }
        pthread_mutex_unlock(&b->lock);
        if (!b->key) return; /* no record for this pair */
    }
}
