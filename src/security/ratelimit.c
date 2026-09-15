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
 * collapsing every real client into the proxy's single bucket. Trust is
 * evaluated on the peer ADDRESS, not on the hashed bucket key, so a proxy
 * reached over IPv4 or IPv6 can be trusted with "1.2.3.4", "10.0.0.0/8",
 * "::1" or "2001:db8::/32" alike.
 *
 * 0 rps disables everything. */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <netinet/in.h>
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
 * to supply a real client IP, so X-Forwarded-For from it is ignored. Each
 * entry is family-tagged and holds a 16-byte network-order address with its
 * prefix mask (IPv4 uses the first 4 bytes, the rest stay zero). Addresses are
 * canonicalised (AND-ed with the mask) at parse time so matching is a plain
 * masked compare. IPv4 and IPv6 peers are both supported. */
struct trusted_net {
    int family;                 /* AF_INET or AF_INET6 */
    unsigned char addr[16];     /* network order, already masked */
    unsigned char mask[16];     /* prefix mask, same layout */
};
static struct trusted_net g_trusted[RATE_TRUSTED_MAX];
static int g_trusted_n = 0;

/* Integer mix (splitmix-style) so adjacent IPs spread across the table
 * instead of clustering in the low buckets of a naive `ip % N`. */
static unsigned int mix_ip(unsigned int x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Fill a 16-byte prefix mask: `prefix` leading 1 bits over `nbytes` bytes. */
static void build_mask(unsigned char *mask, int prefix, int nbytes) {
    memset(mask, 0, 16);
    for (int i = 0; i < nbytes; i++) {
        if (prefix >= 8) { mask[i] = 0xff; prefix -= 8; }
        else if (prefix > 0) { mask[i] = (unsigned char)(0xffu << (8 - prefix)); prefix = 0; }
        else break;
    }
}

/* Strict decimal prefix: rejects "", "abc", "1a", negatives and anything over
 * 128. Returns -1 on malformed input — the caller drops the entry, so a typo
 * can never silently widen the trusted set (atoi would map "abc" to 0, i.e.
 * "trust everyone"). */
static int parse_prefix(const char *s) {
    if (!*s) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 128) return -1;
    }
    return v;
}

/* Parse one entry — "a.b.c.d", "a.b.c.d/n", "::1", "2001:db8::/32" or the
 * bracketed "[::1]" / "[::1]/128" form — into a family-tagged network. The
 * stored address is AND-ed with its mask so rate_limit_is_trusted() is a
 * plain masked compare. Returns 1 on success. */
static int parse_cidr(const char *s, struct trusted_net *out) {
    char buf[64];
    size_t n = 0;
    while (*s && *s != ',' && n + 1 < sizeof(buf)) buf[n++] = *s++;
    buf[n] = '\0';

    /* trim surrounding blanks */
    char *t = buf;
    while (*t == ' ' || *t == '\t') t++;
    size_t tl = strlen(t);
    while (tl > 0 && (t[tl - 1] == ' ' || t[tl - 1] == '\t')) t[--tl] = '\0';
    if (!*t) return 0;

    /* Optional [..] around an IPv6 literal. */
    char *addr = t;
    const char *tail = NULL;
    if (*addr == '[') {
        char *close = strchr(addr, ']');
        if (!close) return 0;
        *close = '\0';                       /* terminate the literal */
        addr++;
        tail = close + 1;                    /* "" or "/nnn" */
    }

    /* Split off the prefix. For a bracketed entry the literal is already
     * NUL-terminated; a bare "addr/nnn" must be cut at the '/' or inet_aton
     * would see the prefix as trailing junk (BSD inet_aton rejects it). */
    int prefix = -1;
    if (tail) {
        if (*tail) {
            if (*tail != '/') return 0;      /* junk after "]" */
            prefix = parse_prefix(tail + 1);
            if (prefix < 0) return 0;
        }
    } else {
        char *slash = strchr(addr, '/');
        if (slash) {
            *slash = '\0';
            prefix = parse_prefix(slash + 1);
            if (prefix < 0) return 0;
        }
    }

    if (strchr(addr, ':')) {                 /* IPv6 literal */
        struct in6_addr a6;
        if (inet_pton(AF_INET6, addr, &a6) != 1) return 0;
        if (prefix < 0) prefix = 128;
        if (prefix > 128) return 0;
        out->family = AF_INET6;
        build_mask(out->mask, prefix, 16);
        memcpy(out->addr, &a6, 16);
        for (int i = 0; i < 16; i++) out->addr[i] &= out->mask[i];
        return 1;
    }

    struct in_addr a4;
    if (inet_aton(addr, &a4) == 0) return 0;
    if (prefix < 0) prefix = 32;
    if (prefix > 32) return 0;
    out->family = AF_INET;
    build_mask(out->mask, prefix, 4);
    memset(out->addr, 0, 16);
    memcpy(out->addr, &a4, 4);
    for (int i = 0; i < 4; i++) out->addr[i] &= out->mask[i];
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
        if (parse_cidr(entry, &g_trusted[g_trusted_n])) {
            g_trusted_n++;
        }
        if (*p == ',') p++;
    }
}

int rate_limit_is_trusted(const struct sockaddr *sa, socklen_t len) {
    (void)len;
    if (!sa) return 0;
    const unsigned char *bytes;
    int nbytes;
    if (sa->sa_family == AF_INET) {
        bytes = (const unsigned char *)&((const struct sockaddr_in *)sa)->sin_addr;
        nbytes = 4;
    } else if (sa->sa_family == AF_INET6) {
        bytes = (const unsigned char *)&((const struct sockaddr_in6 *)sa)->sin6_addr;
        nbytes = 16;
    } else {
        return 0;
    }
    for (int i = 0; i < g_trusted_n; i++) {
        if (g_trusted[i].family != sa->sa_family) continue;
        int match = 1;
        for (int j = 0; j < nbytes; j++) {
            if ((bytes[j] & g_trusted[i].mask[j]) != g_trusted[i].addr[j]) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

/* Derive the 32-bit bucket key from a v4/v6 address: the IPv4 address as-is
 * (network order, matching struct sockaddr_in.sin_addr.s_addr) so a direct
 * peer and an X-Forwarded-For hop compare identically, or an FNV-1a hash of
 * the 128-bit IPv6 address so each v6 client lands in one consistent bucket.
 * Never returns 0 for a valid address (0 means "no key" to callers). */
static unsigned int key_from_addr(int family, const void *addr) {
    if (family == AF_INET) {
        unsigned int v;
        memcpy(&v, addr, sizeof v);
        return v;
    }
    if (family == AF_INET6) {
        const unsigned char *b = (const unsigned char *)addr;
        unsigned int h = 0x811c9dc5u; /* FNV-1a 32-bit offset basis */
        for (int i = 0; i < 16; i++) { h ^= b[i]; h *= 0x01000193u; }
        return h ? h : 1u; /* never return 0: 0 means "no key" to callers */
    }
    return 0;
}

unsigned int rate_limit_parse_xff(const char *xff) {
    if (!xff) return 0;
    /* The original client is the LEFTmost address; downstream proxies append
     * to the right. Take the token up to the first comma. */
    char buf[128];
    size_t n = 0;
    while (*xff && *xff != ',' && n + 1 < sizeof(buf)) {
        if (*xff != ' ' && *xff != '\t') buf[n++] = *xff;
        xff++;
    }
    buf[n] = '\0';
    if (!n) return 0;

    char *p = buf;
    if (*p == '[') {                /* [2001:db8::1] or [2001:db8::1]:port */
        char *close = strchr(p, ']');
        if (!close) return 0;
        *close = '\0';
        p++;
        struct in6_addr a6;
        if (inet_pton(AF_INET6, p, &a6) != 1) return 0;
        return key_from_addr(AF_INET6, &a6);
    }
    int colons = 0;
    for (const char *q = p; *q; q++) { if (*q == ':') colons++; }
    if (colons > 1) {               /* bare IPv6 literal */
        struct in6_addr a6;
        if (inet_pton(AF_INET6, p, &a6) != 1) return 0;
        return key_from_addr(AF_INET6, &a6);
    }
    if (colons == 1) {              /* "1.2.3.4:5678" -> drop the port */
        *strchr(p, ':') = '\0';
    }
    struct in_addr a4;
    if (inet_aton(p, &a4) == 0) return 0;
    return key_from_addr(AF_INET, &a4);
}

/* Bucket key for a peer sockaddr (see key_from_addr). 0 = unknown family. */
static unsigned int sockaddr_key(const struct sockaddr *sa, socklen_t len) {
    (void)len;
    if (!sa) return 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        return key_from_addr(AF_INET, &sin->sin_addr);
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        return key_from_addr(AF_INET6, &sin6->sin6_addr);
    }
    return 0;
}

unsigned int rate_limit_key_from_peer(const struct sockaddr *sa, socklen_t len,
                                      const HttpRequest *req) {
    unsigned int peer = sockaddr_key(sa, len);
    if (peer && req && req->x_forwarded_for[0] && rate_limit_is_trusted(sa, len)) {
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
