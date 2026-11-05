/* DB-driver smoke for the optional remote backends (WITH_MYSQL=1 /
 * WITH_PG=1). Connects to the backend named by argv[1] (a mysql:// or
 * postgres:// dsn), runs argv[2] (default "SELECT 1") through the real
 * driver layer and prints the JSON result. Exit 0 proves the backend
 * compiled AND reached a live server — the CI ubuntu-db leg runs this
 * against freshly started mysqld + postgres.
 *
 * Standalone: no server required to BUILD, so it lives in its own target
 * (make test-db-smoke) rather than test-unit, which must stay
 * dependency-free.
 */

#include <stdio.h>
#include <stdlib.h>

#include "db_layer.h"
#include "tools.h" /* ToolFn, tools_register signature */

/* db_layer.c reuses db_validate_read()/db_validate_write() from
 * sqlite_tool.c for its PostgreSQL and MySQL backends, and dispatches a
 * plain/empty DSN to sqlite_query_json() there as well, so this standalone
 * binary has to link that module. Its sqlite_tools_init() registers four
 * chat tools through tools_register(), which lives in src/agent/tools.c
 * alongside most of the agent framework — pulling that in just to satisfy
 * the linker would drag the whole server into a smoke test. t_db_smoke
 * only calls db_query_json() with a mysql:// or postgres:// DSN and never
 * calls sqlite_tools_init(), so a no-op registration sink is enough.
 * tests/test_sqlite_guard.c stubs the same symbol for the same
 * standalone-link reason. */
int tools_register(const char *name, const char *desc, const char *params_json,
                   ToolFn fn, void *data) {
    (void)name; (void)desc; (void)params_json; (void)fn; (void)data;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: t_db_smoke <dsn> [sql]\n");
        return 2;
    }
    const char *dsn = argv[1];
    const char *sql = argc > 2 ? argv[2] : "SELECT 1";
    sbuf out = {0};
    char err[512] = "";
    if (db_query_json(dsn, sql, NULL, 0, &out, err, sizeof(err)) != 0) {
        fprintf(stderr, "db_query_json(%s) failed: %s\n", dsn, err);
        free(out.p);
        return 1;
    }
    printf("%s -> %s\n", dsn, out.p ? out.p : "");
    free(out.p);
    return 0;
}
