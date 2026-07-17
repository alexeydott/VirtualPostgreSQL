#include "sqlite3.h"

#include <stdio.h>
#include <string.h>

static int vps_exec_expect(sqlite3 *database, const char *sql)
{
    char *error_message = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &error_message);

    if (result != SQLITE_OK) {
        (void)fprintf(stderr,
                      "[host] level=error operation=exec result=%d class=sqlite\n",
                      result);
        sqlite3_free(error_message);
        return 0;
    }
    return 1;
}

static int vps_query_text(sqlite3 *database,
                          const char *sql,
                          const char *expected)
{
    sqlite3_stmt *statement = NULL;
    const unsigned char *value;
    int result;
    int passed = 0;

    result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_text(statement, 0);
        passed = value != NULL && strcmp((const char *)value, expected) == 0;
    }
    if (!passed) {
        (void)fprintf(stderr,
                      "[host] level=error operation=query class=unexpected-result\n");
    }
    (void)sqlite3_finalize(statement);
    return passed;
}

int main(int argument_count, char **arguments)
{
    sqlite3 *database = NULL;
    char *error_message = NULL;
    int result;
    int passed = 1;
    const char *expected_architecture = sizeof(void *) == 4 ? "x86" : "x64";

    if (argument_count != 2) {
        (void)fprintf(stderr,
                      "[host] level=error operation=arguments status=invalid\n");
        return 2;
    }
    result = sqlite3_open(":memory:", &database);
    if (result != SQLITE_OK) {
        return 1;
    }
    passed &= sqlite3_enable_load_extension(database, 1) == SQLITE_OK;
    result = sqlite3_load_extension(database, arguments[1],
                                    "sqlite3_virtualpostgresql_init",
                                    &error_message);
    if (result != SQLITE_OK) {
        (void)fprintf(stderr,
                      "[host] level=error operation=load result=%d class=sqlite\n",
                      result);
        sqlite3_free(error_message);
        passed = 0;
    }
    if (passed) {
        passed &= vps_query_text(database,
                                 "SELECT virtualpostgresql_version()",
                                 "0.1.0-stage1");
        passed &= vps_query_text(database,
                                 "SELECT virtualpostgresql_build_arch()",
                                 expected_architecture);
        passed &= vps_query_text(database,
                                 "SELECT virtualpostgresql_libpq_version()",
                                 "not-linked-stage1");
        passed &= vps_query_text(database,
                                 "SELECT virtualpostgresql_embedded_sqlite()",
                                 "not-linked-stage1");
        passed &= vps_exec_expect(
            database,
            "CREATE VIRTUAL TABLE temp.vps_stage1 USING VirtualPostgreSQL");
        passed &= vps_query_text(database,
                                 "SELECT CAST(count(*) AS TEXT) FROM vps_stage1",
                                 "0");
        passed &= vps_query_text(database, "PRAGMA integrity_check", "ok");
    }
    result = sqlite3_close(database);
    passed &= result == SQLITE_OK;
    (void)printf(
        "[host] level=info operation=load-unload sqlite_version=%s arch=%s "
        "module_version=4 integrity=enabled status=%s\n",
        sqlite3_libversion(), expected_architecture,
        passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
