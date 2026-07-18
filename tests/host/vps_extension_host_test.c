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
        "SELECT g::pg_catalog.int8 AS id "
        "FROM pg_catalog.generate_series(1,10000) AS g");
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
    passed &= vps_expect(row_count == 10000U, "real-stream-row-count", database);
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
        if (runtime_connstr != NULL && runtime_connstr[0] != '\0') {
            passed &= vps_runtime_contour(database, runtime_connstr, cancel);
            if (vps_environment_enabled("VPS_VTAB_TEST_DML"))
                passed &= vps_dml_contour(database, runtime_connstr);
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
