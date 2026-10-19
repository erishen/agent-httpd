/* Unit tests for the SQL write guard in src/agent/sqlite_tool.c
 * (db_validate_read / db_validate_write). Mirrors lume-core's
 * tests/sqlite.lume coverage of the same guard lineage: the strictly-shaped
 * additive ALTER TABLE ... ADD COLUMN allowance (2490034), the portable
 * case-insensitive keyword scan (30fca2a) and the dangerous-DDL /
 * portfolio-mirror / WHERE-clause rules. Compiled standalone with the module
 * under ASan/UBSan (see Makefile test-unit); no database file needed — the
 * guard is pure string analysis. */

#include <stdio.h>
#include <string.h>

#include "db_layer.h"
#include "tools.h" /* ToolFn, tools_register signature */

/* The guard lives in sqlite_tool.c, whose sqlite_tools_init() registers the
 * chat tools via tools_register(); the test links the module standalone, so
 * the registration sink is stubbed out (never called — main() only exercises
 * db_validate_read/write). */
int tools_register(const char *name, const char *desc, const char *params_json,
                   ToolFn fn, void *data) {
    (void)name; (void)desc; (void)params_json; (void)fn; (void)data;
    return 0;
}

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            printf("PASS: %s\n", #cond);                                \
        } else {                                                        \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__);            \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* Run the write guard; returns the error message or NULL. */
static const char *wguard(const char *sql) {
    static char body[8192];
    return db_validate_write(sql, body, sizeof body);
}

static const char *rguard(const char *sql) {
    static char body[8192];
    return db_validate_read(sql, body, sizeof body);
}

int main(void) {
    /* --- read guard -------------------------------------------------- */
    CHECK(rguard("SELECT 1") == NULL);
    CHECK(rguard("  select * from t;") == NULL);
    CHECK(rguard("DELETE FROM t") != NULL);          /* non-SELECT refused */
    CHECK(rguard("SELECT 1; SELECT 2") != NULL);     /* multi-statement */
    CHECK(rguard("-- c\nSELECT 1") == NULL);         /* comment stripped */

    /* --- allowed DML / DDL ------------------------------------------- */
    CHECK(wguard("INSERT INTO t(a) VALUES(1)") == NULL);
    CHECK(wguard("UPDATE t SET a=1 WHERE id=2") == NULL);
    CHECK(wguard("DELETE FROM t WHERE id=2") == NULL);
    CHECK(wguard("CREATE TABLE t9(a TEXT)") == NULL);

    /* UPDATE/DELETE must carry WHERE (case-insensitive) */
    CHECK(wguard("UPDATE t SET a=1") != NULL);
    CHECK(wguard("DELETE FROM t") != NULL);
    CHECK(wguard("UPDATE t SET a=1 where id=2") == NULL); /* lowercase where */

    /* --- dangerous DDL still rejected -------------------------------- */
    CHECK(wguard("DROP TABLE t") != NULL);
    CHECK(wguard("TRUNCATE TABLE t") != NULL);
    CHECK(wguard("PRAGMA journal_mode") != NULL);
    CHECK(wguard("ATTACH DATABASE 'x' AS y") != NULL);
    CHECK(wguard("VACUUM") != NULL);

    /* keyword-boundary: a column named "dropdown" must not trip DROP */
    CHECK(wguard("INSERT INTO t(dropdown) VALUES(1)") == NULL);

    /* --- ALTER: only the strictly-shaped additive form passes -------- */
    CHECK(wguard("ALTER TABLE t ADD COLUMN c TEXT") == NULL);
    CHECK(wguard("alter table t add c INTEGER") == NULL);  /* COLUMN optional */
    CHECK(wguard("ALTER TABLE t ADD c VARCHAR(120)") == NULL);
    CHECK(wguard("ALTER TABLE t ADD c DECIMAL(10,2)") == NULL);
    /* every other ALTER form stays rejected */
    CHECK(wguard("ALTER TABLE t DROP COLUMN c") != NULL);
    CHECK(wguard("ALTER TABLE t RENAME TO u") != NULL);
    CHECK(wguard("ALTER TABLE t RENAME COLUMN a TO b") != NULL);
    CHECK(wguard("ALTER TABLE t ADD c TEXT DEFAULT 'x'") != NULL); /* literal */
    CHECK(wguard("ALTER TABLE t ADD c TEXT; DROP TABLE t") != NULL);
    CHECK(wguard("ALTER TABLE t ADD") != NULL);      /* no column name */
    CHECK(wguard("ALTER INDEX idx RENAME TO j") != NULL);

    /* --- portfolio mirror stays fully read-only ---------------------- */
    CHECK(wguard("UPDATE portfolio SET a=1 WHERE id=1") != NULL);
    CHECK(wguard("ALTER TABLE portfolio ADD COLUMN c TEXT") != NULL);
    /* fail-closed: underscore-suffixed names escape a word-boundary scan */
    CHECK(wguard("INSERT INTO portfolio_mirror(a) VALUES(1)") != NULL);
    CHECK(wguard("UPDATE portfolio_x SET a=1 WHERE id=1") != NULL);

    printf(failures ? "FAILED: %d check(s)\n" : "ok all guard tests passed\n",
           failures);
    return failures ? 1 : 0;
}
