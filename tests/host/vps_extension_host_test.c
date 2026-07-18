#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
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
                      "[host] level=error operation=query class=unexpected-result sqlite=%s\n",
                      sqlite3_errmsg(database));
    }
    (void)sqlite3_finalize(statement);
    return passed;
}

static int vps_query_positive_integer(sqlite3 *database, const char *sql)
{
    sqlite3_stmt *statement = NULL;
    int passed = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) ==
                     SQLITE_OK &&
                 sqlite3_step(statement) == SQLITE_ROW &&
                 sqlite3_column_int(statement, 0) > 0;
    (void)sqlite3_finalize(statement);
    return passed;
}

static int vps_runtime_contour(sqlite3 *database, const char *connstr)
{
    sqlite3_stmt *left = NULL;
    sqlite3_stmt *right = NULL;
    char *sql;
    int passed = 1;
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_table USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=pg_catalog,table=pg_type,mode=ro,"
        "key_columns=oid)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_view USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=pg_catalog,table=pg_views,mode=ro)",
        connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_query USING VirtualPostgreSQL("
        "connstr=%Q,source=query,query=%Q,mode=ro,key_columns=id)",
        connstr,
        "SELECT 7::pg_catalog.int8 AS id, NULL::pg_catalog.text AS n, "
        "'\\x0041'::pg_catalog.bytea AS b, "
        "'550e8400-e29b-41d4-a716-446655440000'::pg_catalog.uuid AS u");
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_query_text(database,
                             "SELECT CAST(count(*) > 0 AS TEXT) FROM vps_table",
                             "1");
    passed &= vps_query_text(database,
                             "SELECT CAST(count(*) > 0 AS TEXT) FROM vps_view",
                             "1");
    passed &= vps_query_text(
        database,
        "SELECT CAST(id AS TEXT) || ':' || CAST(n IS NULL AS TEXT) || ':' || "
        "hex(b) || ':' || u FROM vps_query",
        "7:1:0041:550e8400-e29b-41d4-a716-446655440000");
    passed &= sqlite3_prepare_v2(database,
                                "SELECT oid FROM vps_table LIMIT 2", -1,
                                &left, NULL) == SQLITE_OK;
    passed &= sqlite3_prepare_v2(database,
                                "SELECT oid FROM vps_table LIMIT 2", -1,
                                &right, NULL) == SQLITE_OK;
    if (left != NULL && right != NULL) {
        passed &= sqlite3_step(left) == SQLITE_ROW;
        passed &= sqlite3_step(right) == SQLITE_ROW;
        passed &= sqlite3_step(left) == SQLITE_ROW;
        passed &= sqlite3_step(right) == SQLITE_ROW;
    }
    (void)sqlite3_finalize(left);
    (void)sqlite3_finalize(right);
    return passed;
}

static char *vps_runtime_connstr(void)
{
#if defined(_WIN32)
    char *value = NULL;
    size_t length = 0U;
    if (_dupenv_s(&value, &length, "VPS_VTAB_TEST_CONNSTR") != 0)
        return NULL;
    return value;
#else
    const char *environment = getenv("VPS_VTAB_TEST_CONNSTR");
    size_t length;
    char *value;
    if (environment == NULL) return NULL;
    length = strlen(environment) + 1U;
    value = (char *)malloc(length);
    if (value != NULL) (void)memcpy(value, environment, length);
    return value;
#endif
}

int main(int argument_count, char **arguments)
{
    sqlite3 *database = NULL;
    char *error_message = NULL;
    int result;
    int passed = 1;
    const char *expected_architecture = sizeof(void *) == 4 ? "x86" : "x64";
    char *runtime_connstr = vps_runtime_connstr();

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
                                 "0.7.0");
        passed &= vps_query_text(database,
                                 "SELECT virtualpostgresql_build_arch()",
                                 expected_architecture);
        passed &= vps_query_positive_integer(
            database, "SELECT virtualpostgresql_libpq_version()");
        passed &= vps_query_text(database,
                                 "SELECT virtualpostgresql_embedded_sqlite()",
                                 SQLITE_VERSION);
        passed &= vps_query_text(
            database,
            "SELECT CAST(instr(virtualpostgresql_capabilities(), 'single-row') > 0 AS TEXT)",
            "1");
        passed &= vps_exec_expect(
            database,
            "SELECT count(*) FROM virtualpostgresql_table_info");
        result = sqlite3_exec(
            database,
            "CREATE VIRTUAL TABLE temp.vps_missing USING VirtualPostgreSQL",
            NULL, NULL, &error_message);
        passed &= result != SQLITE_OK;
        sqlite3_free(error_message);
        error_message = NULL;
        passed &= vps_query_text(database, "PRAGMA integrity_check", "ok");
        if (runtime_connstr != NULL && runtime_connstr[0] != '\0')
            passed &= vps_runtime_contour(database, runtime_connstr);
    }
    result = sqlite3_close(database);
    passed &= result == SQLITE_OK;
    free(runtime_connstr);
    (void)printf(
        "[host] level=info operation=load-unload sqlite_version=%s arch=%s "
        "module_version=4 integrity=enabled status=%s\n",
        sqlite3_libversion(), expected_architecture,
        passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
