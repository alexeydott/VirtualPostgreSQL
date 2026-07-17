#include "vps_proto_common.h"

#include <openssl/sha.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed, non-sensitive Stage 0 fixtures. No user-provided SQL is accepted or logged. */
static const char VPS_SQL_ZERO[] = "SELECT 42::int4 AS fixed_value";
static const char VPS_SQL_METADATA[] = "SELECT $1::int4 AS value_i4, $2::bytea AS value_bytes";
static const char VPS_SQL_BYTEA[] = "SELECT $1::bytea AS value_bytes";
static const char VPS_SQL_INVALID_OID[] = "SELECT $1";
static const char VPS_SQL_SINGLE[] = "SELECT g::int4 FROM generate_series(1, 100) AS g";
static const char VPS_SQL_LATE_ERROR[] = "SELECT CASE WHEN g < 4 THEN g::int4 ELSE (1 / (g - g))::int4 END FROM generate_series(1, 5) AS g";

typedef struct vps_query_context {
    unsigned int result_count;
    unsigned int clear_count;
} vps_query_context;

static PGresult *vps_next_result(PGconn *connection,
                                 const vps_deadline *deadline,
                                 vps_query_context *context,
                                 int *operation_ok)
{
    PGresult *result = vps_get_result_async(connection, deadline, NULL, operation_ok);
    if (result != NULL) {
        context->result_count++;
    }
    return result;
}

static void vps_clear_result(PGresult **result, vps_query_context *context)
{
    if (result != NULL && *result != NULL) {
        PQclear(*result);
        *result = NULL;
        context->clear_count++;
    }
}

static int vps_expect_terminal_null(PGconn *connection,
                                    const vps_deadline *deadline,
                                    vps_query_context *context)
{
    int operation_ok = 0;
    PGresult *result = vps_next_result(connection, deadline, context, &operation_ok);
    if (!operation_ok || result != NULL) {
        vps_clear_result(&result, context);
        return 0;
    }
    return 1;
}

static int vps_prepare(PGconn *connection,
                       const char *name,
                       const char *sql,
                       int parameter_count,
                       const Oid *parameter_types,
                       const vps_deadline *deadline,
                       vps_query_context *context,
                       ExecStatusType expected_status)
{
    int operation_ok = 0;
    PGresult *result;
    ExecStatusType status;
    char fields[128];

    if (PQsendPrepare(connection, name, sql, parameter_count, parameter_types) == 0 ||
        !vps_flush_async(connection, deadline, NULL)) {
        return 0;
    }
    result = vps_next_result(connection, deadline, context, &operation_ok);
    if (!operation_ok || result == NULL) {
        vps_clear_result(&result, context);
        return 0;
    }
    status = PQresultStatus(result);
    (void)snprintf(fields, sizeof(fields), "operation=prepare status=%s parameter_count=%d sqlstate=%s",
                   vps_result_status_name(status), parameter_count, vps_sqlstate(result));
    vps_log(status == expected_status ? "info" : "error", "query_result", fields);
    vps_clear_result(&result, context);
    return status == expected_status && vps_expect_terminal_null(connection, deadline, context);
}

static int vps_run_metadata(PGconn *connection,
                            const vps_deadline *deadline,
                            vps_query_context *context)
{
    static const Oid types[2] = { 23U, 17U };
    const uint32_t integer_value = htonl(7U);
    static const unsigned char bytes[4] = { 0U, 1U, 0U, 255U };
    const char *values[2];
    int lengths[2];
    int formats[2] = { 1, 1 };
    int operation_ok = 0;
    PGresult *result = NULL;
    ExecStatusType status;
    int ok = 1;
    int field;

    if (!vps_prepare(connection, "vps_zero", VPS_SQL_ZERO, 0, NULL, deadline, context, PGRES_COMMAND_OK)) {
        return 0;
    }
    if (PQsendQueryPrepared(connection, "vps_zero", 0, NULL, NULL, NULL, 1) == 0 ||
        !vps_flush_async(connection, deadline, NULL)) {
        return 0;
    }
    result = vps_next_result(connection, deadline, context, &operation_ok);
    ok = operation_ok && result != NULL && PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) == 1;
    vps_clear_result(&result, context);
    if (!ok || !vps_expect_terminal_null(connection, deadline, context)) {
        return 0;
    }

    if (!vps_prepare(connection, "vps_metadata", VPS_SQL_METADATA, 2, types, deadline, context, PGRES_COMMAND_OK)) {
        return 0;
    }
    if (PQsendDescribePrepared(connection, "vps_metadata") == 0 || !vps_flush_async(connection, deadline, NULL)) {
        return 0;
    }
    result = vps_next_result(connection, deadline, context, &operation_ok);
    if (!operation_ok || result == NULL || PQresultStatus(result) != PGRES_COMMAND_OK ||
        PQnparams(result) != 2 || PQnfields(result) != 2) {
        vps_clear_result(&result, context);
        return 0;
    }
    for (field = 0; field < PQnfields(result); ++field) {
        char fields[192];
        (void)snprintf(fields, sizeof(fields), "operation=describe field=%d name=%s oid=%u typmod=%d format=%d",
                       field, PQfname(result, field), (unsigned int)PQftype(result, field), PQfmod(result, field), PQfformat(result, field));
        vps_log("info", "field_metadata", fields);
    }
    ok = PQparamtype(result, 0) == 23U && PQparamtype(result, 1) == 17U &&
         PQftype(result, 0) == 23U && PQftype(result, 1) == 17U;
    vps_clear_result(&result, context);
    if (!ok || !vps_expect_terminal_null(connection, deadline, context)) {
        return 0;
    }

    values[0] = (const char *)&integer_value;
    values[1] = (const char *)bytes;
    lengths[0] = 4;
    lengths[1] = (int)sizeof(bytes);
    if (PQsendQueryPrepared(connection, "vps_metadata", 2, values, lengths, formats, 1) == 0 ||
        !vps_flush_async(connection, deadline, NULL)) {
        return 0;
    }
    result = vps_next_result(connection, deadline, context, &operation_ok);
    status = result == NULL ? PGRES_FATAL_ERROR : PQresultStatus(result);
    ok = operation_ok && result != NULL && status == PGRES_TUPLES_OK && PQntuples(result) == 1 &&
         PQnfields(result) == 2 && PQfformat(result, 0) == 1 && PQfformat(result, 1) == 1 &&
         PQgetlength(result, 0, 0) == 4 && PQgetlength(result, 0, 1) == (int)sizeof(bytes) &&
         memcmp(PQgetvalue(result, 0, 0), &integer_value, 4U) == 0 &&
         memcmp(PQgetvalue(result, 0, 1), bytes, sizeof(bytes)) == 0;
    vps_clear_result(&result, context);
    return ok && vps_expect_terminal_null(connection, deadline, context);
}

static int vps_run_invalid_count(PGconn *connection,
                                 const vps_deadline *deadline,
                                 vps_query_context *context)
{
    static const Oid types[2] = { 23U, 17U };
    static const char value[4] = { 0, 0, 0, 1 };
    const char *values[1] = { value };
    int lengths[1] = { 4 };
    int formats[1] = { 1 };
    int operation_ok = 0;
    PGresult *result;
    int ok;
    char fields[128];

    if (!vps_prepare(connection, "vps_bad_count", VPS_SQL_METADATA, 2, types, deadline, context, PGRES_COMMAND_OK)) {
        return 0;
    }
    if (PQsendQueryPrepared(connection, "vps_bad_count", 1, values, lengths, formats, 1) == 0 ||
        !vps_flush_async(connection, deadline, NULL)) {
        return 0;
    }
    result = vps_next_result(connection, deadline, context, &operation_ok);
    ok = operation_ok && result != NULL && PQresultStatus(result) == PGRES_FATAL_ERROR && PQntuples(result) == 0;
    (void)snprintf(fields, sizeof(fields), "operation=execute_mismatch status=%s sqlstate=%s published_rows=0",
                   result == NULL ? "none" : vps_result_status_name(PQresultStatus(result)), vps_sqlstate(result));
    vps_log(ok ? "info" : "error", "query_expected_error", fields);
    vps_clear_result(&result, context);
    return ok && vps_expect_terminal_null(connection, deadline, context);
}

static int vps_run_invalid_oid(PGconn *connection,
                               const vps_deadline *deadline,
                               vps_query_context *context)
{
    static const Oid invalid_type[1] = { 99999999U };
    return vps_prepare(connection, "vps_bad_oid", VPS_SQL_INVALID_OID, 1, invalid_type,
                       deadline, context, PGRES_FATAL_ERROR);
}

static int vps_send_bytea(PGconn *connection,
                          const unsigned char *payload,
                          size_t length,
                          int is_null,
                          int input_format,
                          const vps_deadline *deadline,
                          vps_query_context *context,
                          int expect_success)
{
    static const Oid type[1] = { 17U };
    const char *values[1];
    int lengths[1];
    int formats[1];
    int operation_ok = 0;
    PGresult *result;
    ExecStatusType status;
    int ok;

    if (length > (size_t)INT_MAX) {
        return !expect_success;
    }
    values[0] = is_null ? NULL : (const char *)payload;
    lengths[0] = (int)length;
    formats[0] = input_format;
    if (PQsendQueryParams(connection, VPS_SQL_BYTEA, 1, type, values, lengths, formats, 1) == 0 ||
        !vps_flush_async(connection, deadline, NULL)) {
        return 0;
    }
    result = vps_next_result(connection, deadline, context, &operation_ok);
    status = result == NULL ? PGRES_FATAL_ERROR : PQresultStatus(result);
    if (expect_success) {
        ok = operation_ok && result != NULL && status == PGRES_TUPLES_OK && PQntuples(result) == 1 &&
             (is_null ? PQgetisnull(result, 0, 0) != 0 :
                        (PQgetisnull(result, 0, 0) == 0 && PQgetlength(result, 0, 0) == (int)length &&
                         memcmp(PQgetvalue(result, 0, 0), payload, length) == 0));
    } else {
        ok = operation_ok && result != NULL && status == PGRES_FATAL_ERROR;
    }
    vps_clear_result(&result, context);
    return ok && vps_expect_terminal_null(connection, deadline, context);
}

static void vps_sha256_hex(const unsigned char *data, size_t length, char output[65])
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    size_t index;

    (void)SHA256(data, length, digest);
    for (index = 0U; index < SHA256_DIGEST_LENGTH; ++index) {
        (void)snprintf(output + index * 2U, 3U, "%02x", digest[index]);
    }
    output[64] = '\0';
}

static int vps_run_bytea(PGconn *connection,
                         const vps_deadline *deadline,
                         vps_query_context *context)
{
    static const size_t sizes[] = { 0U, 7U, 65535U, 65536U, 1048576U };
    unsigned char empty = 0U;
    size_t case_index;

    if (!vps_send_bytea(connection, NULL, 0U, 1, 1, deadline, context, 1) ||
        !vps_send_bytea(connection, &empty, 0U, 0, 1, deadline, context, 1)) {
        return 0;
    }
    vps_log("info", "bytea_null_empty", "null=distinct empty_length=0 outcome=pass");
    for (case_index = 1U; case_index < sizeof(sizes) / sizeof(sizes[0]); ++case_index) {
        size_t index;
        size_t length = sizes[case_index];
        unsigned char *payload = (unsigned char *)malloc(length);
        char hash[65];
        char fields[160];
        int ok;

        if (payload == NULL) {
            return 0;
        }
        for (index = 0U; index < length; ++index) {
            payload[index] = (unsigned char)((index * 131U + case_index) & 0xffU);
        }
        if (length > 2U) {
            payload[1] = 0U;
        }
        vps_sha256_hex(payload, length, hash);
        ok = vps_send_bytea(connection, payload, length, 0, 1, deadline, context, 1);
        (void)snprintf(fields, sizeof(fields), "format=binary bytes=%zu sha256=%s outcome=%s",
                       length, hash, ok ? "pass" : "fail");
        vps_log(ok ? "info" : "error", "bytea_roundtrip", fields);
        free(payload);
        if (!ok) {
            return 0;
        }
    }
    {
        static const unsigned char malformed[] = "\\x0";
        if (!vps_send_bytea(connection, malformed, sizeof(malformed) - 1U, 0, 0, deadline, context, 0)) {
            return 0;
        }
    }
    {
        static const char text_value[] = "\\x000100ff";
        static const unsigned char expected[] = { 0U, 1U, 0U, 255U };
        static const Oid type[1] = { 17U };
        const char *values[1] = { text_value };
        int formats[1] = { 0 };
        int operation_ok = 0;
        PGresult *result;
        int ok;
        if (PQsendQueryParams(connection, VPS_SQL_BYTEA, 1, type, values, NULL, formats, 1) == 0 ||
            !vps_flush_async(connection, deadline, NULL)) { return 0; }
        result = vps_next_result(connection, deadline, context, &operation_ok);
        ok = operation_ok && result != NULL && PQresultStatus(result) == PGRES_TUPLES_OK &&
             PQgetlength(result, 0, 0) == (int)sizeof(expected) &&
             memcmp(PQgetvalue(result, 0, 0), expected, sizeof(expected)) == 0;
        vps_clear_result(&result, context);
        if (!ok || !vps_expect_terminal_null(connection, deadline, context)) { return 0; }
        vps_log("info", "bytea_format_compare", "input=text output=binary bytes=4 outcome=pass");
    }
    vps_log("info", "bytea_size_guard", "boundary=INT_MAX one_over=rejected");
    return 1;
}

static int vps_run_single(PGconn *connection,
                          const vps_deadline *deadline,
                          vps_query_context *context,
                          int late_error)
{
    const char *sql = late_error ? VPS_SQL_LATE_ERROR : VPS_SQL_SINGLE;
    uint64_t started_at = (uint64_t)GetTickCount64();
    uint64_t first_row_ms = 0U;
    unsigned int rows = 0U;
    unsigned int expected_value = 1U;
    unsigned int terminal_count = 0U;
    int saw_expected_error = 0;
    int operation_ok = 0;
    ExecStatusType last_status = (ExecStatusType)-1;
    PGresult *result;
    char fields[192];

    if (PQsendQueryParams(connection, sql, 0, NULL, NULL, NULL, NULL, 1) == 0) {
        return 0;
    }
    if (PQsetSingleRowMode(connection) == 0 || !vps_flush_async(connection, deadline, NULL)) {
        return 0;
    }
    for (;;) {
        ExecStatusType status;
        result = vps_next_result(connection, deadline, context, &operation_ok);
        if (!operation_ok) {
            vps_clear_result(&result, context);
            return 0;
        }
        if (result == NULL) {
            break;
        }
        status = PQresultStatus(result);
        if (status != last_status) {
            (void)snprintf(fields, sizeof(fields), "status=%s rows=%u result_count=%u",
                           vps_result_status_name(status), rows, context->result_count);
            vps_log("debug", "single_row_transition", fields);
            last_status = status;
        }
        if (status == PGRES_SINGLE_TUPLE) {
            uint32_t network_value;
            unsigned int value;
            if (PQntuples(result) != 1 || PQnfields(result) != 1 || PQgetlength(result, 0, 0) != 4) {
                vps_clear_result(&result, context);
                return 0;
            }
            memcpy(&network_value, PQgetvalue(result, 0, 0), 4U);
            value = ntohl(network_value);
            if (value != expected_value) {
                vps_clear_result(&result, context);
                return 0;
            }
            expected_value++;
            rows++;
            if (first_row_ms == 0U) {
                first_row_ms = (uint64_t)GetTickCount64() - started_at;
            }
        } else {
            terminal_count++;
            if (late_error && status == PGRES_FATAL_ERROR && strcmp(vps_sqlstate(result), "22012") == 0) {
                saw_expected_error = 1;
            } else if (!late_error && status != PGRES_TUPLES_OK) {
                vps_clear_result(&result, context);
                return 0;
            }
        }
        vps_clear_result(&result, context);
    }
    (void)snprintf(fields, sizeof(fields), "rows=%u first_row_ms=%llu terminal_count=%u late_error=%s sqlstate=%s retry=false",
                   rows, (unsigned long long)first_row_ms, terminal_count,
                   late_error ? "true" : "false", saw_expected_error ? "22012" : "none");
    vps_log("info", "single_row_complete", fields);
    if (late_error) {
        return rows == 3U && terminal_count == 1U && saw_expected_error;
    }
    return rows == 100U && terminal_count == 1U;
}

static int vps_run_mode(const char *mode,
                        PGconn *connection,
                        const vps_deadline *deadline,
                        vps_query_context *context)
{
    if (strcmp(mode, "metadata") == 0) { return vps_run_metadata(connection, deadline, context); }
    if (strcmp(mode, "invalid-count") == 0) { return vps_run_invalid_count(connection, deadline, context); }
    if (strcmp(mode, "invalid-oid") == 0) { return vps_run_invalid_oid(connection, deadline, context); }
    if (strcmp(mode, "bytea") == 0 || strcmp(mode, "null-empty") == 0) { return vps_run_bytea(connection, deadline, context); }
    if (strcmp(mode, "single-success") == 0) { return vps_run_single(connection, deadline, context, 0); }
    if (strcmp(mode, "late-error") == 0) { return vps_run_single(connection, deadline, context, 1); }
    return 0;
}

int main(int argc, char **argv)
{
    static const char *const all_modes[] = {
        "metadata", "invalid-count", "invalid-oid", "bytea", "single-success", "late-error"
    };
    PGconn *connection;
    vps_deadline deadline = vps_deadline_after(30000U);
    vps_query_context context = { 0U, 0U };
    int ok = 1;
    size_t index;
    char fields[128];

    if (argc != 2) {
        vps_log("error", "usage", "modes=metadata,invalid-count,invalid-oid,null-empty,bytea,single-success,late-error,all");
        return 64;
    }
    connection = vps_connect_test_server(5000U);
    if (connection == NULL) {
        return 65;
    }
    if (strcmp(argv[1], "all") == 0) {
        for (index = 0U; index < sizeof(all_modes) / sizeof(all_modes[0]); ++index) {
            if (!vps_run_mode(all_modes[index], connection, &deadline, &context)) {
                ok = 0;
                break;
            }
        }
    } else {
        ok = vps_run_mode(argv[1], connection, &deadline, &context);
    }
    if (PQtransactionStatus(connection) != PQTRANS_IDLE || PQisBusy(connection) != 0) {
        ok = 0;
    }
    PQfinish(connection);
    (void)snprintf(fields, sizeof(fields), "outcome=%s results=%u clears=%u cleanup_exact=%s",
                   ok ? "pass" : "fail", context.result_count, context.clear_count,
                   context.result_count == context.clear_count ? "true" : "false");
    vps_log(ok && context.result_count == context.clear_count ? "info" : "error", "query_suite_complete", fields);
    return ok && context.result_count == context.clear_count ? 0 : 1;
}
