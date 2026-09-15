/* ---- Per-IP rate limiting (-l <rps>, RATE_LIMIT_RPS) ----
 *
 * Token-bucket counters in POSIX shared memory so every process (fork-per-
 * connection AND prefork workers) enforces the same limit. One bucket per IP,
 * found by a hashed index with short linear probing so two IPs that collide
 * no longer clobber each other's accounting (the old "steal-the-bucket"
 * behaviour let a later arrival wipe the previous occupant's counter).
 *
 * The bucket key is a 32-bit value. For IPv4 it is the address in NETWORK
 * byte order, matching struct sockaddr_in.sin_addr.s_addr, so a direct peer
 * and an X-Forwarded-For hop compare identically. For IPv6 it is a stable
 * FNV-1a hash of the 128-bit address, so each client maps to one consistent
 * bucket instead of every v6 client collapsing into bucket 0.
 * rate_limit_key_from_peer() derives this key from a generic sockaddr.
 *
 * When the direct peer is a TRUSTED proxy (configured via
 * rate_limit_set_trusted_proxies from RATE_LIMIT_TRUSTED_PROXIES), the first
 * hop of X-Forwarded-For becomes the limiting key; otherwise the peer IP is
 * used. This keeps the limiter correct behind a reverse proxy instead of
 * collapsing every real client into the proxy's single bucket.
 *
 * 0 rps disables everything. */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <arpa/inet.h>

#include "internal.h"

#define RATE_TABLE_SIZE 4096        /* P3: more buckets than the old 1024 */
#define RATE_MAX_PROBES 8          /* P1: probe chain before degrading */
#define RATE_TRUSTED_MAX 32        /* max trusted-proxy CIDR entries */

int g_rate_limit_rps = 0; /* disabled by default */

struct rate_bucket {
    pthread_mutex_t lock;
    unsigned int ip;   /* network-order key this bucket currently serves */
    long long ts;      /* last token refill (seconds, time(NULL)) */
    int tokens;        /* current tokens (token bucket) */
    int used;          /* 0 = never claimed, 1 = claimed (stays claimed) */
};

static struct rate_bucket *g_rate_table = NULL; /* mmap(MAP_SHARED|MAP_ANON) */

/* Trusted reverse-proxy subnets. A peer outside every entry is never trusted
 * to supply a real client IP, so X-Forwarded-For from it is ignored. Both
 * fields are IPv4 in NETWORK byte order, matching the bucket keys. */
static struct {
    unsigned int ip;
    unsigned int mask;
} g_trusted[RATE_TRUSTED_MAX];
static int g_trusted_n = 0;

/* Integer mix (splitmix-style) so adjacent IPs spread across the table
 * instead of clustering in the low buckets of a naive `ip % N`. */
static unsigned int mix_ip(unsigned int x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Parse "a.b.c.d" or "a.b.c.d/n" (n = prefix length 0..32) into a
 * network-order address + network-order mask. Returns 1 on success. */
static int parse_cidr(const char *s, unsigned int *out_ip, unsigned int *out_mask) {
    char buf[64];
    size_t n = 0;
    while (*s && *s != ',' && n + 1 < sizeof(buf)) buf[n++] = *s++;
    buf[n] = '\0';
    char *slash = strchr(buf, '/');
    int prefix = 32;
    if (slash) {
        *slash = '\0';
        prefix = atoi(slash + 1);
        if (prefix < 0 || prefix > 32) return 0;
    }
    struct in_addr a;
    if (inet_aton(buf, &a) == 0) return 0;
    unsigned int mask;
    if (prefix == 0) mask = 0;
    else {
        unsigned int host_mask = ~((1u << (32 - prefix)) - 1) & 0xFFFFFFFFu;
        mask = htonl(host_mask);
    }
    *out_ip = a.s_addr;       /* network order */
    *out_mask = mask;         /* network order */
    return 1;
}

void rate_limit_set_trusted_proxies(const char *csv) {
    g_trusted_n = 0;
    if (!csv) return;
    const char *p = csv;
    while (*p && g_trusted_n < RATE_TRUSTED_MAX) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t len = (size_t)(p - start);
        char entry[64];
        if (len >= sizeof(entry)) { if (*p == ',') p++; continue; }
        memcpy(entry, start, len);
        entry[len] = '\0';
        unsigned int ip, mask;
        if (parse_cidr(entry, &ip, &mask)) {
            g_trusted[g_trusted_n].ip = ip;
            g_trusted[g_trusted_n].mask = mask;
            g_trusted_n++;
        }
        if (*p == ',') p++;
    }
}

int rate_limit_is_trusted(unsigned int peer_ip) {
    for (int i = 0; i < g_trusted_n; i++) {
        if ((peer_ip & g_trusted[i].mask) == (g_trusted[i].ip & g_trusted[i].mask))
            return 1;
    }
    return 0;
}

unsigned int rate_limit_parse_xff(const char *xff) {
    if (!xff) return 0;
    /* The original client is the LEFTmost address; downstream proxies append
     * to the right. Take the substring up to the first comma. */
    char buf[64];
    size_t n = 0;
    while (*xff && *xff != ',' && n + 1 < sizeof(buf)) {
        if (*xff != ' ' && *xff != '\t') buf[n++] = *xff;
        xff++;
    }
    buf[n] = '\0';
    if (!n) return 0;
    struct in_addr a;
    if (inet_aton(buf, &a) == 0) return 0;
    return a.s_addr; /* network order, matches the bucket key */
}

/* Derive the 32-bit bucket key for a peer address: the IPv4 address as-is, or
 * an FNV-1a hash of the 128-bit IPv6 address. When the peer is a TRUSTED proxy
 * (rate_limit_is_trusted) supplying X-Forwarded-For, the first hop replaces the
 * key (IPv4 only today: trusted proxies sit in front of IPv4 clients). Returns
 * 0 for an unknown/absent family. */
static unsigned int sockaddr_key(const struct sockaddr *sa, socklen_t len) {
    (void)len;
    if (!sa) return 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        return sin->sin_addr.s_addr;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        const unsigned char *b = sin6->sin6_addr.s6_addr;
        unsigned int h = 0x811c9dc5u; /* FNV-1a 32-bit offset basis */
        for (int i = 0; i < 16; i++) { h ^= b[i]; h *= 0x01000193u; }
        return h ? h : 1u; /* never return 0: 0 means "no key" to callers */
    }
    return 0;
}

unsigned int rate_limit_key_from_peer(const struct sockaddr *sa, socklen_t len,
                                      const HttpRequest *req) {
    unsigned int peer = sockaddr_key(sa, len);
    if (peer && req && req->x_forwarded_for[0] && rate_limit_is_trusted(peer)) {
        unsigned int x = rate_limit_parse_xff(req->x_forwarded_for);
        if (x) return x;
    }
    return peer;
}

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

/* Refill + (maybe) consume one token on bucket b. Caller holds b->lock.
 * Returns 1 if the request is allowed, 0 if the bucket is empty. */
static int bucket_spend(struct rate_bucket *b, int cap, long long now) {
    if (now > b->ts) {
        b->tokens += (int)(now - b->ts) * cap;
        if (b->tokens > cap) b->tokens = cap;
        b->ts = now;
    }
    if (b->tokens >= 1) {
        b->tokens -= 1;
        return 1;
    }
    return 0;
}

/* Returns 1 when allowed, 0 when this IP exhausted its quota this second. */
int rate_limit_allow(unsigned int ip) {
    if (g_rate_limit_rps <= 0 || !g_rate_table) return 1;
    int cap = g_rate_limit_rps;
    unsigned int base = mix_ip(ip) % RATE_TABLE_SIZE;
    long long now = (long long)time(NULL);

    for (int i = 0; i < RATE_MAX_PROBES; i++) {
        struct rate_bucket *b = &g_rate_table[(base + i) % RATE_TABLE_SIZE];
        pthread_mutex_lock(&b->lock);
        if (!b->used) {
            /* empty slot: claim it and consume the first token */
            b->ip = ip;
            b->ts = now;
            b->tokens = cap - 1;
            b->used = 1;
            pthread_mutex_unlock(&b->lock);
            return 1;
        }
        if (b->ip == ip) {
            int ok = bucket_spend(b, cap, now);
            pthread_mutex_unlock(&b->lock);
            return ok;
        }
        /* occupied by a DIFFERENT, live IP: never clobber its counter, just
         * probe the next slot. */
        pthread_mutex_unlock(&b->lock);
    }

    /* Every probe slot is occupied by another live IP (extremely rare given
     * 4096 buckets x 8 probes). Degrade gracefully: share the base slot
     * without overwriting its owner/accounting, so we never erase someone
     * else's token count. */
    struct rate_bucket *b = &g_rate_table[base];
    pthread_mutex_lock(&b->lock);
    int ok = bucket_spend(b, cap, now);
    pthread_mutex_unlock(&b->lock);
    return ok;
}
