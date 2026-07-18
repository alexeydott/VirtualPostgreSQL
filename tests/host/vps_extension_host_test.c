#include "sqlite3.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

typedef int32_t (*VpsHostCancelFn)(sqlite3 *database);

#if defined(VPS_DEBUG)
static void vps_sqlite_log(void *context, int code, const char *message)
{
    (void)context;
    (void)fprintf(stderr,
                  "[host] level=debug operation=sqlite-log code=%d message=%.511s\n",
                  code, message != NULL ? message : "unavailable");
}
#endif

#if defined(_WIN32)
typedef struct VpsCancelThreadContext {
    sqlite3 *database;
    VpsHostCancelFn cancel;
    int32_t result;
} VpsCancelThreadContext;

static DWORD WINAPI vps_cancel_thread(LPVOID opaque)
{
    VpsCancelThreadContext *context = (VpsCancelThreadContext *)opaque;
    Sleep(25U);
    context->result = context->cancel(context->database);
    return 0U;
}
#endif

static int vps_exec_expect(sqlite3 *database, const char *sql)
{
    char *error_message = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &error_message);

    if (result != SQLITE_OK) {
        (void)fprintf(stderr,
                      "[host] level=error operation=exec result=%d class=sqlite message=%.255s\n",
                      result,
                      error_message != NULL && error_message[0] != '\0'
                          ? error_message : sqlite3_errmsg(database));
        sqlite3_free(error_message);
        return 0;
    }
    return 1;
}

static int vps_exec_expect_failure(sqlite3 *database, const char *sql,
                                   int expected_primary)
{
    char *error_message = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &error_message);
    int passed = result != SQLITE_OK &&
                 (expected_primary == 0 ||
                  (result & 0xff) == expected_primary);
    if (!passed)
        (void)fprintf(stderr,
                      "[host] level=error operation=expected-failure result=%d message=%.255s\n",
                      result, error_message != NULL ? error_message
                                                    : sqlite3_errmsg(database));
    sqlite3_free(error_message);
    return passed;
}

static int vps_expect(int condition, const char *operation, sqlite3 *database)
{
    if (!condition) {
        (void)fprintf(stderr,
                      "[host] level=error operation=%s class=unexpected-result sqlite=%.255s\n",
                      operation, sqlite3_errmsg(database));
    }
    return condition;
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
                      "[host] level=error operation=query class=unexpected-result expected=%s sqlite=%s\n",
                      expected, sqlite3_errmsg(database));
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

static int vps_query_contains(sqlite3 *database,
                              const char *sql,
                              const char *expected)
{
    sqlite3_stmt *statement = NULL;
    const unsigned char *value;
    int passed = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) ==
                 SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW;
    value = passed ? sqlite3_column_text(statement, 3) : NULL;
    passed = value != NULL && strstr((const char *)value, expected) != NULL;
    (void)sqlite3_finalize(statement);
    return passed;
}

static int vps_query_first_column_contains(sqlite3 *database,
                                           const char *sql,
                                           const char *expected)
{
    sqlite3_stmt *statement = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK)
        return 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char *value = sqlite3_column_text(statement, 0);
        if (value != NULL && strstr((const char *)value, expected) != NULL) {
            found = 1;
            break;
        }
    }
    (void)sqlite3_finalize(statement);
    return found;
}

static int vps_environment_enabled(const char *name)
{
#if defined(_WIN32)
    char *value = NULL;
    size_t length = 0U;
    int enabled;
    if (_dupenv_s(&value, &length, name) != 0) return 0;
    enabled = value != NULL && strcmp(value, "1") == 0;
    free(value);
    return enabled;
#else
    const char *value = getenv(name);
    return value != NULL && strcmp(value, "1") == 0;
#endif
}

#if defined(_WIN32)
static size_t vps_peak_working_set(void)
{
    PROCESS_MEMORY_COUNTERS counters;
    (void)memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                              sizeof(counters)))
        return 0U;
    return (size_t)counters.PeakWorkingSetSize;
}
#endif

static int vps_runtime_bulk_contour(sqlite3 *database, const char *connstr)
{
    enum { VPS_BULK_CURSOR_COUNT = 8, VPS_BULK_CYCLES = 1000 };
    const size_t stream_rows =
        vps_environment_enabled("VPS_VTAB_TEST_PERFORMANCE")
            ? 1000000U : 10000U;
    sqlite3_stmt *statements[VPS_BULK_CURSOR_COUNT];
    sqlite3_stmt *stream = NULL;
    char *sql;
    size_t row_count = 0U;
    size_t cursor_index;
    size_t cycle;
    int step_result = SQLITE_ERROR;
    int passed = 1;
#if defined(_WIN32)
    ULONGLONG started = GetTickCount64();
    ULONGLONG first_row_ms = 0U;
    size_t rss_before = vps_peak_working_set();
    size_t rss_after;
#endif

    (void)memset(statements, 0, sizeof(statements));
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_stream USING VirtualPostgreSQL("
        "connstr=%Q,source=query,query=%Q,mode=ro,key_columns=id)",
        connstr,
        vps_environment_enabled("VPS_VTAB_TEST_PERFORMANCE")
            ? "SELECT g::pg_catalog.int8 AS id FROM pg_catalog.generate_series(1,1000000) AS g"
            : "SELECT g::pg_catalog.int8 AS id FROM pg_catalog.generate_series(1,10000) AS g");
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_expect(
        sqlite3_prepare_v2(database, "SELECT id FROM vps_stream", -1,
                           &stream, NULL) == SQLITE_OK,
        "real-stream-prepare", database);
    while (stream != NULL && (step_result = sqlite3_step(stream)) == SQLITE_ROW) {
        ++row_count;
#if defined(_WIN32)
        if (row_count == 1U) first_row_ms = GetTickCount64() - started;
#endif
    }
    passed &= vps_expect(stream != NULL && step_result == SQLITE_DONE,
                         "real-stream-terminal", database);
    passed &= vps_expect(row_count == stream_rows, "real-stream-row-count", database);
    (void)sqlite3_finalize(stream);
#if defined(_WIN32)
    rss_after = vps_peak_working_set();
    passed &= vps_expect(rss_after <= rss_before + 64U * 1024U * 1024U,
                         "real-stream-rss-bound", database);
    (void)printf(
        "[host] level=info operation=real-stream rows=%llu first_row_ms=%llu duration_ms=%llu peak_rss_delta=%llu status=%s\n",
        (unsigned long long)row_count, (unsigned long long)first_row_ms,
        (unsigned long long)(GetTickCount64() - started),
        (unsigned long long)(rss_after > rss_before ? rss_after - rss_before
                                                    : 0U),
        passed ? "passed" : "failed");
#endif

    for (cursor_index = 0U; cursor_index < VPS_BULK_CURSOR_COUNT;
         ++cursor_index) {
        passed &= vps_expect(
            sqlite3_prepare_v2(database,
                               "SELECT id FROM vps_planner ORDER BY id LIMIT 1",
                               -1, &statements[cursor_index], NULL) == SQLITE_OK,
            "concurrency-prepare", database);
    }
    for (cycle = 0U; cycle < VPS_BULK_CYCLES && passed; ++cycle) {
        for (cursor_index = 0U; cursor_index < VPS_BULK_CURSOR_COUNT;
             ++cursor_index)
            passed &= vps_expect(sqlite3_step(statements[cursor_index]) ==
                                     SQLITE_ROW,
                                 "concurrency-row", database);
        for (cursor_index = 0U; cursor_index < VPS_BULK_CURSOR_COUNT;
             ++cursor_index) {
            passed &= vps_expect(sqlite3_step(statements[cursor_index]) ==
                                     SQLITE_DONE,
                                 "concurrency-terminal", database);
            passed &= vps_expect(sqlite3_reset(statements[cursor_index]) ==
                                     SQLITE_OK,
                                 "concurrency-reset", database);
        }
    }
    for (cursor_index = 0U; cursor_index < VPS_BULK_CURSOR_COUNT;
         ++cursor_index)
        (void)sqlite3_finalize(statements[cursor_index]);
    passed &= vps_query_text(database,
                             "SELECT CAST(count(*) > 0 AS TEXT) FROM vps_table",
                             "1");
    (void)printf(
        "[host] level=info operation=concurrency cursors=%u cycles=%u scans=%u status=%s\n",
        (unsigned int)VPS_BULK_CURSOR_COUNT, (unsigned int)VPS_BULK_CYCLES,
        (unsigned int)(VPS_BULK_CURSOR_COUNT * VPS_BULK_CYCLES),
        passed ? "passed" : "failed");
    return passed;
}

static int vps_runtime_contour(sqlite3 *database,
                               const char *connstr,
                               VpsHostCancelFn cancel)
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
    passed &= vps_query_text(
        database,
        "SELECT CAST((SELECT count(*) FROM temp.vps_table_vps_schema)=1 "
        "AND (SELECT count(*) FROM temp.vps_table_vps_metadata)=1 AS TEXT)",
        "1");
    sql = sqlite3_mprintf(
        "SELECT CAST(count(*)>0 AS TEXT) FROM "
        "virtualpostgresql_relations(%Q,'pg_catalog')", connstr);
    if (sql == NULL) return 0;
    passed &= vps_expect(vps_query_text(database, sql, "1"),
                         "metadata-relations", database); sqlite3_free(sql);
    sql = sqlite3_mprintf(
        "SELECT CAST(count(*)>0 AS TEXT) FROM "
        "virtualpostgresql_table_info(%Q,'pg_catalog','pg_type')", connstr);
    if (sql == NULL) return 0;
    passed &= vps_expect(vps_query_text(database, sql, "1"),
                         "metadata-table-info", database); sqlite3_free(sql);
    sql = sqlite3_mprintf(
        "SELECT CAST(count(*)>0 AS TEXT) FROM "
        "virtualpostgresql_index_list(%Q,'pg_catalog','pg_type')", connstr);
    if (sql == NULL) return 0;
    passed &= vps_expect(vps_query_text(database, sql, "1"),
                         "metadata-index-list", database); sqlite3_free(sql);
    sql = sqlite3_mprintf(
        "SELECT CAST(count(*)>0 AS TEXT) FROM "
        "virtualpostgresql_index_info(%Q,'pg_catalog','pg_type','pg_type_oid_index')",
        connstr);
    if (sql == NULL) return 0;
    passed &= vps_expect(vps_query_text(database, sql, "1"),
                         "metadata-index-info", database); sqlite3_free(sql);
    sql = sqlite3_mprintf(
        "SELECT CAST(count(*)=1 AS TEXT) FROM "
        "virtualpostgresql_type_info(%Q,'pg_catalog','int4')", connstr);
    if (sql == NULL) return 0;
    passed &= vps_expect(vps_query_text(database, sql, "1"),
                         "metadata-type-info", database); sqlite3_free(sql);
    sql = sqlite3_mprintf(
        "SELECT CAST(count(*)>0 AS TEXT) FROM "
        "virtualpostgresql_extensions(%Q)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_expect(vps_query_text(database, sql, "1"),
                         "metadata-extensions", database); sqlite3_free(sql);
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
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_planner USING VirtualPostgreSQL("
        "connstr=%Q,source=query,query=%Q,mode=ro,key_columns=id)",
        connstr,
        "SELECT * FROM (VALUES "
        "(1::pg_catalog.int8,true,'550e8400-e29b-41d4-a716-446655440000'::pg_catalog.uuid,'b'::pg_catalog.text),"
        "(2::pg_catalog.int8,false,'550e8400-e29b-41d4-a716-446655440001'::pg_catalog.uuid,NULL::pg_catalog.text),"
        "(3::pg_catalog.int8,true,'550e8400-e29b-41d4-a716-446655440002'::pg_catalog.uuid,'a'::pg_catalog.text)) "
        "AS p(id,enabled,u,n)");
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_query_text(database,
                             "SELECT group_concat(id, ',') FROM vps_planner "
                             "WHERE id IN (1,3,NULL) ORDER BY id",
                             "1,3");
    passed &= vps_query_text(database,
                             "SELECT CAST(enabled AS TEXT) || ':' || u "
                             "FROM vps_planner WHERE id=2",
                             "0:550e8400-e29b-41d4-a716-446655440001");
    passed &= vps_query_text(database,
                             "SELECT group_concat(COALESCE(n,'NULL'), ',') "
                             "FROM (SELECT n FROM vps_planner ORDER BY n ASC)",
                             "NULL,a,b");
    passed &= vps_query_text(database,
                             "SELECT CAST(id AS TEXT) FROM vps_planner "
                             "ORDER BY id LIMIT 1 OFFSET 1",
                             "2");
    passed &= vps_query_text(database,
                             "SELECT group_concat(id, ',') FROM "
                             "(SELECT id FROM vps_planner ORDER BY id "
                             "LIMIT -1 OFFSET -5)",
                             "1,2,3");
    passed &= vps_query_text(database,
                             "SELECT group_concat(id, ',') FROM "
                             "(SELECT id FROM vps_planner ORDER BY n DESC "
                             "LIMIT 2)",
                             "1,3");
    passed &= vps_query_text(database,
                             "SELECT group_concat(id, ',') FROM vps_planner "
                             "WHERE id IN (1,'not-an-integer')",
                             "1");
    passed &= vps_query_text(database,
                             "SELECT CAST(count(*) AS TEXT) FROM vps_planner "
                             "WHERE n='b'",
                             "1");
    passed &= vps_query_text(database,
                             "SELECT CAST(count(*) AS TEXT) FROM vps_planner "
                             "WHERE u=2",
                             "0");
    passed &= vps_query_text(database,
                             "SELECT CAST(count(*) AS TEXT) FROM vps_planner "
                             "WHERE enabled=2",
                             "0");
    passed &= vps_query_contains(
        database, "EXPLAIN QUERY PLAN SELECT id FROM vps_planner WHERE id=2",
        "VIRTUAL TABLE INDEX 2:");
    (void)printf(
        "[host] level=info operation=pushdown-equivalence cases=9 unique_plan=1 local_recheck=1 status=%s\n",
        passed ? "passed" : "failed");
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_materialized_memory USING "
        "VirtualPostgreSQL(connstr=%Q,source=query,query=%Q,mode=ro,"
        "key_columns=id,query_indexes=by_id=id;by_label=label,"
        "query_materialization=memory)", connstr,
        "SELECT * FROM (VALUES "
        "(1::pg_catalog.int8,NULL::pg_catalog.text,'1.250'::pg_catalog.numeric,'\\x00ff'::pg_catalog.bytea),"
        "(2::pg_catalog.int8,'two'::pg_catalog.text,'2.500'::pg_catalog.numeric,'\\x'::pg_catalog.bytea),"
        "(3::pg_catalog.int8,'three'::pg_catalog.text,'3.750'::pg_catalog.numeric,'\\x4100'::pg_catalog.bytea)) "
        "AS q(id,label,amount,payload)");
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_materialized_temp USING "
        "VirtualPostgreSQL(connstr=%Q,source=query,query=%Q,mode=ro,"
        "key_columns=id,query_indexes=by_id=id;by_label=label,"
        "query_materialization=temp)", connstr,
        "SELECT * FROM (VALUES "
        "(1::pg_catalog.int8,NULL::pg_catalog.text,'1.250'::pg_catalog.numeric,'\\x00ff'::pg_catalog.bytea),"
        "(2::pg_catalog.int8,'two'::pg_catalog.text,'2.500'::pg_catalog.numeric,'\\x'::pg_catalog.bytea),"
        "(3::pg_catalog.int8,'three'::pg_catalog.text,'3.750'::pg_catalog.numeric,'\\x4100'::pg_catalog.bytea)) "
        "AS q(id,label,amount,payload)");
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_query_text(
        database,
        "SELECT CAST(id AS TEXT)||':'||label||':'||amount||':'||hex(payload) "
        "FROM vps_materialized_memory WHERE id=2", "2:two:2.500:");
    passed &= vps_query_text(
        database,
        "SELECT CAST(count(*) AS TEXT) FROM vps_materialized_memory m "
        "JOIN vps_materialized_temp t USING(id) "
        "WHERE m.label IS t.label AND m.amount=t.amount "
        "AND m.payload IS t.payload", "3");
    passed &= vps_query_text(
        database,
        "SELECT group_concat(id, ',') FROM "
        "(SELECT id FROM vps_materialized_memory ORDER BY id LIMIT 2 OFFSET 1)",
        "2,3");
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

    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_late USING VirtualPostgreSQL("
        "connstr=%Q,source=query,query=%Q,mode=ro,key_columns=id)",
        connstr,
        "SELECT g::pg_catalog.int8 AS id, "
        "(10 / (3-g))::pg_catalog.int8 AS value "
        "FROM pg_catalog.generate_series(1,3) AS g");
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_expect(
        sqlite3_prepare_v2(database, "SELECT id,value FROM vps_late", -1,
                           &left, NULL) == SQLITE_OK,
        "late-error-prepare", database);
    if (left != NULL) {
        passed &= vps_expect(sqlite3_step(left) == SQLITE_ROW,
                             "late-error-row-1", database);
        passed &= vps_expect(sqlite3_step(left) == SQLITE_ROW,
                             "late-error-row-2", database);
        passed &= vps_expect(sqlite3_step(left) == SQLITE_ERROR,
                             "late-error-terminal", database);
    }
    (void)sqlite3_finalize(left);
    left = NULL;

    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_slow USING VirtualPostgreSQL("
        "connstr=%Q,source=query,query=%Q,mode=ro,key_columns=id)",
        connstr,
        "SELECT g::pg_catalog.int8 AS id, "
        "pg_catalog.pg_sleep(0.005) IS NULL AS waited "
        "FROM pg_catalog.generate_series(1,1000000) AS g");
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= sqlite3_prepare_v2(database, "SELECT id FROM vps_slow", -1,
                                 &left, NULL) == SQLITE_OK;
    if (left != NULL) passed &= sqlite3_step(left) == SQLITE_ROW;
    (void)sqlite3_finalize(left);
    left = NULL;
    passed &= vps_query_text(database,
                             "SELECT CAST(count(*) > 0 AS TEXT) FROM "
                             "vps_table",
                             "1");
#if defined(_WIN32)
    if (cancel != NULL) {
        VpsCancelThreadContext cancel_context;
        HANDLE thread;
        int step_result = SQLITE_ROW;
        size_t rows = 0U;
        (void)memset(&cancel_context, 0, sizeof(cancel_context));
        cancel_context.database = database;
        cancel_context.cancel = cancel;
        passed &= sqlite3_prepare_v2(database, "SELECT id FROM vps_slow", -1,
                                     &right, NULL) == SQLITE_OK;
        if (right != NULL) {
            step_result = sqlite3_step(right);
            if (step_result == SQLITE_ROW) rows += 1U;
        }
        thread = step_result == SQLITE_ROW
                     ? CreateThread(NULL, 0U, vps_cancel_thread,
                                    &cancel_context, 0U, NULL)
                     : NULL;
        passed &= thread != NULL;
        while (right != NULL && step_result == SQLITE_ROW) {
            step_result = sqlite3_step(right);
            if (step_result == SQLITE_ROW) rows += 1U;
        }
        if (thread != NULL) {
            passed &= WaitForSingleObject(thread, 5000U) == WAIT_OBJECT_0;
            (void)CloseHandle(thread);
        }
        passed &= vps_expect(cancel_context.result == 0,
                             "explicit-cancel-request", database);
        passed &= vps_expect(rows != 0U, "explicit-cancel-progress", database);
        if (step_result != SQLITE_INTERRUPT)
            (void)fprintf(stderr,
                          "[host] level=error operation=explicit-cancel-code actual=%d expected=%d rows=%llu\n",
                          step_result, SQLITE_INTERRUPT,
                          (unsigned long long)rows);
        passed &= vps_expect(step_result == SQLITE_INTERRUPT,
                             "explicit-cancel-terminal", database);
        (void)sqlite3_finalize(right);
        right = NULL;
        passed &= vps_query_text(database,
                                 "SELECT CAST(count(*) > 0 AS TEXT) FROM "
                                 "vps_table",
                                 "1");
    } else {
        passed = 0;
    }
#else
    (void)cancel;
#endif
    if (vps_environment_enabled("VPS_VTAB_TEST_BULK"))
        passed &= vps_runtime_bulk_contour(database, connstr);
    passed &= vps_exec_expect(
        database,
        "CREATE TEMP TABLE vps_shadow_backup AS SELECT * FROM "
        "temp.vps_table_vps_schema");
    passed &= vps_exec_expect(
        database,
        "UPDATE temp.vps_table_vps_schema SET source_fingerprint='tampered'");
    passed &= vps_expect(
        vps_query_first_column_contains(database, "PRAGMA integrity_check",
                                        "shadow metadata is inconsistent"),
        "metadata-integrity-schema-tamper", database);
    passed &= vps_exec_expect(
        database,
        "DELETE FROM temp.vps_table_vps_schema;INSERT INTO "
        "temp.vps_table_vps_schema SELECT * FROM temp.vps_shadow_backup;");
    passed &= vps_exec_expect(
        database,
        "INSERT INTO temp.vps_table_vps_schema SELECT * FROM "
        "temp.vps_shadow_backup");
    passed &= vps_expect(
        vps_query_first_column_contains(database, "PRAGMA integrity_check",
                                        "shadow metadata is inconsistent"),
        "metadata-integrity-row-count", database);
    passed &= vps_exec_expect(
        database,
        "DELETE FROM temp.vps_table_vps_schema WHERE rowid NOT IN "
        "(SELECT min(rowid) FROM temp.vps_table_vps_schema);"
        "DROP TABLE temp.vps_shadow_backup");
    passed &= vps_exec_expect(
        database,
        "UPDATE temp.vps_table_vps_metadata SET snapshot=x'56505331'");
    passed &= vps_expect(
        vps_query_first_column_contains(database, "PRAGMA integrity_check",
                                        "shadow metadata is inconsistent"),
        "metadata-integrity-tamper", database);
    passed &= vps_exec_expect(database, "DROP TABLE temp.vps_table");
    passed &= vps_query_text(
        database,
        "SELECT CAST(count(*)=0 AS TEXT) FROM temp.sqlite_schema "
        "WHERE name IN ('vps_table_vps_schema','vps_table_vps_metadata')",
        "1");
    return passed;
}

static int vps_dml_contour(sqlite3 *database, const char *connstr)
{
    char *sql;
    int passed = 1;
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_dml USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=public,table=vps_stage11_dml,mode=rw,"
        "optimistic_lock=column,version_column=version)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_exec_expect(
        database, "INSERT INTO vps_dml(__vps_omit) VALUES('*')");
    passed &= vps_query_text(
        database,
        "SELECT payload||':'||version||':'||generated FROM vps_dml "
        "WHERE payload='defaulted'",
        "defaulted:1:defaulted:1");
    passed &= vps_exec_expect(
        database,
        "INSERT INTO vps_dml(payload,nullable,bytes,amount,version,__vps_omit) "
        "VALUES('explicit',NULL,X'0041','12.3400',1,'id,generated')");
    passed &= vps_query_text(
        database,
        "SELECT CAST(nullable IS NULL AS TEXT)||':'||hex(bytes)||':'||amount "
        "FROM vps_dml WHERE payload='explicit'",
        "1:0041:12.3400");
    passed &= vps_exec_expect(
        database,
        "UPDATE vps_dml SET payload='updated',version=version+1 "
        "WHERE payload='explicit'");
    passed &= vps_query_text(
        database,
        "SELECT payload||':'||version||':'||generated FROM vps_dml "
        "WHERE payload='updated'",
        "updated:2:updated:2");
    passed &= vps_exec_expect(database,
                              "DELETE FROM vps_dml WHERE payload='defaulted'");
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_dml_xmin USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=public,table=vps_stage11_dml,mode=rw,"
        "optimistic_lock=xmin)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_exec_expect(
        database,
        "UPDATE vps_dml_xmin SET nullable='xmin-ok' WHERE payload='updated'");
    passed &= vps_query_text(database,
                             "SELECT nullable FROM vps_dml_xmin "
                             "WHERE payload='updated'", "xmin-ok");
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_dml_composite USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=public,"
        "table=vps_stage11_composite,mode=rw)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_exec_expect(
        database,
        "INSERT INTO vps_dml_composite(tenant,code,payload,__vps_omit) "
        "VALUES('a',7,'before','')");
    passed &= vps_exec_expect(
        database,
        "UPDATE vps_dml_composite SET code=8,payload='after' "
        "WHERE tenant='a' AND code=7");
    passed &= vps_query_text(
        database,
        "SELECT tenant||':'||code||':'||payload FROM vps_dml_composite",
        "a:8:after");
    passed &= vps_exec_expect(
        database, "DELETE FROM vps_dml_composite WHERE tenant='a' AND code=8");
    (void)printf("[host] level=info operation=dml-contour status=%s\n",
                 passed ? "passed" : "failed");
    return passed;
}

static int vps_transaction_contour(sqlite3 *database, const char *connstr)
{
    sqlite3_stmt *stream = NULL;
    char *sql;
    int passed = 1;
    int step_result;
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_tx_a USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=public,table=vps_stage11_dml,mode=rw,"
        "optimistic_lock=column,version_column=version)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_tx_b USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=public,table=vps_stage11_composite,mode=rw)",
        connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);

    passed &= vps_exec_expect(database, "BEGIN");
    passed &= vps_exec_expect(
        database,
        "INSERT INTO vps_tx_a(payload,version,__vps_omit) "
        "VALUES('tx-rollback',1,'id,nullable,bytes,amount,generated')");
    passed &= vps_exec_expect(database, "ROLLBACK");
    passed &= vps_query_text(
        database,
        "SELECT CAST(count(*) AS TEXT) FROM vps_tx_a WHERE payload='tx-rollback'",
        "0");

    passed &= vps_exec_expect(database, "BEGIN");
    passed &= vps_exec_expect(
        database,
        "INSERT INTO vps_tx_a(payload,version,__vps_omit) "
        "VALUES('tx-keep',1,'id,nullable,bytes,amount,generated')");
    passed &= vps_exec_expect(database, "SAVEPOINT user_name_is_not_forwarded");
    passed &= vps_exec_expect(
        database, "UPDATE vps_tx_a SET payload='tx-changed',version=2 "
                  "WHERE payload='tx-keep'");
    passed &= vps_exec_expect(database,
                              "ROLLBACK TO user_name_is_not_forwarded");
    passed &= vps_exec_expect(database, "RELEASE user_name_is_not_forwarded");
    passed &= vps_exec_expect(
        database,
        "INSERT INTO vps_tx_b(tenant,code,payload,__vps_omit) "
        "VALUES('tx',1200,'joined','')");
    passed &= vps_exec_expect(database, "COMMIT");
    passed &= vps_query_text(
        database,
        "SELECT payload FROM vps_tx_a WHERE payload IN ('tx-keep','tx-changed')",
        "tx-keep");
    passed &= vps_query_text(
        database, "SELECT payload FROM vps_tx_b WHERE tenant='tx' AND code=1200",
        "joined");

    passed &= vps_exec_expect(database, "BEGIN");
    passed &= vps_exec_expect(
        database, "UPDATE vps_tx_a SET version=version+1 WHERE payload='tx-keep'");
    passed &= vps_exec_expect(database, "SAVEPOINT aborted_recovery");
    passed &= vps_exec_expect_failure(
        database,
        "INSERT OR FAIL INTO vps_tx_b(tenant,code,payload,__vps_omit) "
        "VALUES('tx',1200,'duplicate','')", SQLITE_CONSTRAINT);
    passed &= vps_expect(sqlite3_get_autocommit(database) == 0,
                         "constraint-preserves-savepoint", database);
    passed &= vps_exec_expect(database, "ROLLBACK TO aborted_recovery");
    passed &= vps_exec_expect(database, "RELEASE aborted_recovery");
    passed &= vps_exec_expect(
        database, "UPDATE vps_tx_a SET version=version+1 WHERE payload='tx-keep'");
    passed &= vps_exec_expect(database, "ROLLBACK");

    passed &= vps_exec_expect(database, "BEGIN");
    passed &= vps_exec_expect(
        database, "UPDATE vps_tx_a SET version=version+1 WHERE payload='tx-keep'");
    passed &= sqlite3_prepare_v2(database,
                                "SELECT payload FROM vps_tx_a ORDER BY id",
                                -1, &stream, NULL) == SQLITE_OK;
    step_result = stream != NULL ? sqlite3_step(stream) : SQLITE_ERROR;
    passed &= step_result == SQLITE_ROW;
    passed &= vps_exec_expect_failure(
        database, "DELETE FROM vps_tx_b WHERE tenant='tx' AND code=1200",
        SQLITE_BUSY);
    (void)sqlite3_finalize(stream);
    stream = NULL;
    if (sqlite3_get_autocommit(database) != 0) {
        passed &= vps_exec_expect(database, "BEGIN");
        passed &= vps_exec_expect(
            database,
            "UPDATE vps_tx_a SET version=version+1 WHERE payload='tx-keep'");
    }
    passed &= vps_exec_expect(
        database, "DELETE FROM vps_tx_b WHERE tenant='tx' AND code=1200");
    passed &= vps_exec_expect(database, "ROLLBACK");

    passed &= vps_exec_expect(
        database, "DELETE FROM vps_tx_a WHERE payload='tx-keep'");
    passed &= vps_exec_expect(
        database, "DELETE FROM vps_tx_b WHERE tenant='tx' AND code=1200");
    (void)printf("[host] level=info operation=transaction-contour status=%s\n",
                 passed ? "passed" : "failed");
    return passed;
}

static int vps_spatial_contour(sqlite3 *database, const char *connstr)
{
    char *sql;
    int passed = 1;
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_spatial_wkt USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=public,table=vps_stage13_spatial,"
        "mode=rw,geometry=wkt,srid=4326)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_query_text(
        database,
        "SELECT geom||':'||geog FROM vps_spatial_wkt WHERE id=1",
        "POINT Z (30 10 5):POINT(-71.060316 48.432044)");
    passed &= vps_query_text(
        database,
        "SELECT CAST(nullable_geom IS NULL AS TEXT)||':'||empty_geom "
        "FROM vps_spatial_wkt WHERE id=1",
        "1:GEOMETRYCOLLECTION EMPTY");
    passed &= vps_exec_expect(
        database,
        "INSERT INTO vps_spatial_wkt(id,label,geom,geog,nullable_geom,"
        "empty_geom,__vps_omit) VALUES(101,'roundtrip',"
        "'POINT Z (11 12 13)','POINT(14 15)',NULL,'POINT EMPTY','')");
    passed &= vps_query_text(
        database,
        "SELECT geom||':'||geog||':'||CAST(nullable_geom IS NULL AS TEXT)||"
        "':'||empty_geom FROM vps_spatial_wkt WHERE id=101",
        "POINT Z (11 12 13):POINT(14 15):1:POINT EMPTY");
    passed &= vps_exec_expect(
        database,
        "UPDATE vps_spatial_wkt SET geom='POINT Z (21 22 23)' WHERE id=101");
    passed &= vps_query_text(database,
                             "SELECT geom FROM vps_spatial_wkt WHERE id=101",
                             "POINT Z (21 22 23)");
    passed &= vps_exec_expect_failure(
        database,
        "UPDATE vps_spatial_wkt SET geom='POINT Z (NaN 2 3)' WHERE id=101",
        SQLITE_MISMATCH);

    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_spatial_wkb USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=public,table=vps_stage13_spatial,"
        "mode=rw,geometry=wkb,srid=4326)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_query_text(
        database,
        "SELECT CAST(typeof(geom) AS TEXT)||':'||CAST(length(geom)>20 AS TEXT) "
        "FROM vps_spatial_wkb WHERE id=1", "blob:1");
    passed &= vps_exec_expect(
        database,
        "INSERT INTO vps_spatial_wkb(id,label,geom,geog,__vps_omit) VALUES("
        "102,'wkb',X'01E90300000000000000003E4000000000000024400000000000001440',"
        "X'01010000003CDBA337DCC351C06D37C1374D374840',"
        "'nullable_geom,empty_geom')");
    passed &= vps_query_text(
        database,
        "SELECT CAST((SELECT hex(geom) FROM vps_spatial_wkb WHERE id=1)="
        "hex(geom) AS TEXT) FROM vps_spatial_wkb WHERE id=102", "1");

    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_spatial_ewkt_query USING VirtualPostgreSQL("
        "connstr=%Q,source=query,query=%Q,mode=ro,key_columns=id,"
        "geometry=ewkt,srid=4326)", connstr,
        "SELECT id,geom,geog FROM public.vps_stage13_spatial WHERE id=1");
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_query_text(
        database,
        "SELECT CAST(instr(geom,'SRID=4326;')=1 AS TEXT)||':'||"
        "CAST(instr(geog,'SRID=4326;')=1 AS TEXT) FROM vps_spatial_ewkt_query",
        "1:1");
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_spatial_ewkt USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=public,table=vps_stage13_spatial,"
        "mode=rw,geometry=ewkt,srid=4326)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_exec_expect(
        database,
        "INSERT INTO vps_spatial_ewkt(id,label,geom,geog,__vps_omit) VALUES("
        "103,'ewkt','SRID=4326;POINT Z (30 10 5)',"
        "'SRID=4326;POINT(-71.060316 48.432044)',"
        "'nullable_geom,empty_geom')");
    passed &= vps_query_text(
        database, "SELECT CAST(instr(geom,'SRID=4326;')=1 AS TEXT) "
                  "FROM vps_spatial_ewkt WHERE id=103", "1");

    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_spatial_ewkb USING VirtualPostgreSQL("
        "connstr=%Q,source=table,schema=public,table=vps_stage13_spatial,"
        "mode=rw,geometry=ewkb,srid=4326)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect(database, sql);
    sqlite3_free(sql);
    passed &= vps_query_text(
        database,
        "SELECT CAST(typeof(geom) AS TEXT)||':'||CAST(length(geom)>24 AS TEXT) "
        "FROM vps_spatial_ewkb WHERE id=1", "blob:1");
    passed &= vps_exec_expect(
        database,
        "INSERT INTO vps_spatial_ewkb(id,label,geom,geog,__vps_omit) VALUES("
        "104,'ewkb',X'01010000A0E61000000000000000003E4000000000000024400000000000001440',"
        "X'0101000020E61000003CDBA337DCC351C06D37C1374D374840',"
        "'nullable_geom,empty_geom')");
    passed &= vps_query_text(
        database,
        "SELECT CAST((SELECT hex(geom) FROM vps_spatial_ewkb WHERE id=1)="
        "hex(geom) AS TEXT) FROM vps_spatial_ewkb WHERE id=104", "1");
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_spatialite_unavailable USING "
        "VirtualPostgreSQL(connstr=%Q,source=table,schema=public,"
        "table=vps_stage13_spatial,mode=ro,geometry=spatialite)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect_failure(database, sql, SQLITE_ERROR);
    sqlite3_free(sql);
    sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE temp.vps_spatial_bad_srid USING "
        "VirtualPostgreSQL(connstr=%Q,source=table,schema=public,"
        "table=vps_stage13_spatial,mode=ro,geometry=wkt,srid=3857)", connstr);
    if (sql == NULL) return 0;
    passed &= vps_exec_expect_failure(database, sql, SQLITE_ERROR);
    sqlite3_free(sql);
    passed &= vps_exec_expect(
        database, "DELETE FROM vps_spatial_wkt WHERE id BETWEEN 101 AND 104");
    (void)printf("[host] level=info operation=spatial-contour status=%s\n",
                 passed ? "passed" : "failed");
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
    VpsHostCancelFn cancel = NULL;
#if defined(_WIN32)
    HMODULE extension_module = NULL;
#endif

    if (argument_count != 2) {
        (void)fprintf(stderr,
                      "[host] level=error operation=arguments status=invalid\n");
        return 2;
    }
#if defined(VPS_DEBUG)
    if (sqlite3_config(SQLITE_CONFIG_LOG, vps_sqlite_log, NULL) != SQLITE_OK)
        return 1;
#endif
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
#if defined(_WIN32)
    if (passed) {
        extension_module = LoadLibraryA(arguments[1]);
        if (extension_module != NULL)
            cancel = (VpsHostCancelFn)(void *)GetProcAddress(
                extension_module, "virtualpostgresql_cancel");
        passed &= extension_module != NULL && cancel != NULL;
    }
#endif
    if (passed) {
        passed &= vps_query_text(database,
                                 "SELECT virtualpostgresql_version()",
                                 "0.9.0");
        passed &= vps_query_text(database,
                                 "SELECT virtualpostgresql_build_arch()",
                                 expected_architecture);
        passed &= vps_query_positive_integer(
            database, "SELECT virtualpostgresql_libpq_version()");
        passed &= vps_query_text(
            database,
            "SELECT CAST((instr(virtualpostgresql_capabilities(),"
            "'query-materialization-memory')>0 AND "
            "virtualpostgresql_embedded_sqlite()=sqlite_version()) OR "
            "(instr(virtualpostgresql_capabilities(),"
            "'query-materialization-memory')=0 AND "
            "virtualpostgresql_embedded_sqlite()='disabled') AS TEXT)",
            "1");
        passed &= vps_query_text(
            database,
            "SELECT CAST(instr(virtualpostgresql_capabilities(), 'single-row') > 0 AS TEXT)",
            "1");
        passed &= vps_query_text(
            database,
            "SELECT CAST(instr(virtualpostgresql_capabilities(), 'planner') > 0 AS TEXT)",
            "1");
        passed &= vps_query_text(
            database,
            "SELECT CAST(instr(virtualpostgresql_capabilities(), "
            "'metadata-functions') > 0 AND "
            "instr(virtualpostgresql_capabilities(), 'metadata-cache') > 0 "
            "AS TEXT)", "1");
        passed &= vps_query_text(
            database,
            "SELECT CAST(count(*)=28 AND sum(hidden)=3 AS TEXT) FROM "
            "pragma_table_xinfo('virtualpostgresql_table_info')", "1");
        passed &= vps_exec_expect(
            database,
            "CREATE VIEW temp.vps_metadata_unsafe AS SELECT * FROM "
            "virtualpostgresql_extensions('not-a-connection')");
        passed &= vps_exec_expect_failure(
            database, "SELECT * FROM temp.vps_metadata_unsafe", SQLITE_AUTH);
        passed &= vps_exec_expect(database, "DROP VIEW temp.vps_metadata_unsafe");
        passed &= vps_exec_expect_failure(
            database,
            "SELECT count(*) FROM virtualpostgresql_table_info",
            SQLITE_CONSTRAINT);
        result = sqlite3_exec(
            database,
            "CREATE VIRTUAL TABLE temp.vps_missing USING VirtualPostgreSQL",
            NULL, NULL, &error_message);
        passed &= result != SQLITE_OK;
        sqlite3_free(error_message);
        error_message = NULL;
        passed &= vps_query_text(database, "PRAGMA integrity_check", "ok");
        if (runtime_connstr != NULL && runtime_connstr[0] != '\0') {
            passed &= vps_runtime_contour(database, runtime_connstr, cancel);
            if (vps_environment_enabled("VPS_VTAB_TEST_DML"))
                passed &= vps_dml_contour(database, runtime_connstr);
            if (vps_environment_enabled("VPS_VTAB_TEST_TRANSACTIONS"))
                passed &= vps_transaction_contour(database, runtime_connstr);
            if (vps_environment_enabled("VPS_VTAB_TEST_SPATIAL"))
                passed &= vps_spatial_contour(database, runtime_connstr);
        }
    }
    result = sqlite3_close(database);
    passed &= result == SQLITE_OK;
    free(runtime_connstr);
#if defined(_WIN32)
    if (extension_module != NULL) (void)FreeLibrary(extension_module);
#endif
    (void)printf(
        "[host] level=info operation=load-unload sqlite_version=%s arch=%s "
        "module_version=4 integrity=enabled status=%s\n",
        sqlite3_libversion(), expected_architecture,
        passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
