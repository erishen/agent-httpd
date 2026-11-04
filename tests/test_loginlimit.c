/* Unit tests for the per-(user, ip) login limiter (loginlimit.c).
 *
 * The limiter's only time source is time(NULL), so the whole module is
 * #included after redirecting time() to a controllable fake clock. That lets
 * the tests drive lockout backoff and streak-window expiry without sleeping.
 *
 * Build: cc tests/test_loginlimit.c -Isrc -pthread -o tests/t_loginlimit
 * (mingw additionally needs the winsock stub set and -lws2_32 for
 * internal.h's socket includes; see the Makefile test-unit section for the
 * canonical link line.)
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

static long long fake_now = 1000000000LL;
static time_t fake_time(time_t *t) {
    if (t) *t = (time_t)fake_now;
    return (time_t)fake_now;
}
#define time(t) fake_time(t)

#include "../src/security/loginlimit.c"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* N consecutive failures spaced 1s apart (well inside the 900s window). */
static void fail_n(const char *user, const char *ip, int n) {
    for (int i = 0; i < n; i++) {
        login_limiter_fail(user, ip);
        fake_now += 1;
    }
}

int main(void) {
    login_limiter_init();
    CHECK(g_login_table != NULL);

    /* untouched pair: never locked */
    CHECK(login_limiter_blocked_for("alice", "1.1.1.1") == 0);

    /* 4 failures: below the threshold, still not locked */
    fail_n("alice", "1.1.1.1", 4);
    CHECK(login_limiter_blocked_for("alice", "1.1.1.1") == 0);

    /* 5th failure: locked for ~30s (backoff base) */
    login_limiter_fail("alice", "1.1.1.1");
    fake_now += 1;
    long w1 = login_limiter_blocked_for("alice", "1.1.1.1");
    CHECK(w1 > 25 && w1 <= 30);

    /* another failure while locked: backoff doubles to ~60s */
    login_limiter_fail("alice", "1.1.1.1");
    fake_now += 1;
    long w2 = login_limiter_blocked_for("alice", "1.1.1.1");
    CHECK(w2 > 55 && w2 <= 60);

    /* independent keys: different ip or different user is not affected */
    CHECK(login_limiter_blocked_for("alice", "2.2.2.2") == 0);
    CHECK(login_limiter_blocked_for("bob", "1.1.1.1") == 0);

    /* a correct login clears the pair entirely */
    login_limiter_success("alice", "1.1.1.1");
    CHECK(login_limiter_blocked_for("alice", "1.1.1.1") == 0);
    fail_n("alice", "1.1.1.1", 5);
    CHECK(login_limiter_blocked_for("alice", "1.1.1.1") > 0);

    /* streak window: after 900s without a failure, the count restarts, so a
     * single new failure must NOT lock (with the stale count it would). */
    login_limiter_success("alice", "1.1.1.1");
    fail_n("alice", "1.1.1.1", 4);
    fake_now += LOGIN_RESET_SECONDS + 1;
    login_limiter_fail("alice", "1.1.1.1"); /* would be #5 if not reset */
    fake_now += 1;
    CHECK(login_limiter_blocked_for("alice", "1.1.1.1") == 0);
    /* and a fresh 5-failure streak does lock again */
    fail_n("alice", "1.1.1.1", 4);
    CHECK(login_limiter_blocked_for("alice", "1.1.1.1") > 0);

    /* backoff caps at LOGIN_MAX_LOCKOUT (900s): push a streak far past the
     * threshold and the remaining lockout stays <= 900 */
    login_limiter_success("bob", "9.9.9.9");
    fail_n("bob", "9.9.9.9", 30);
    long wcap = login_limiter_blocked_for("bob", "9.9.9.9");
    CHECK(wcap > 0 && wcap <= LOGIN_MAX_LOCKOUT);

    /* lockout expires with the clock */
    fake_now += LOGIN_MAX_LOCKOUT + 100;
    CHECK(login_limiter_blocked_for("bob", "9.9.9.9") == 0);

    if (failures) {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("all loginlimit tests passed\n");
    return 0;
}
