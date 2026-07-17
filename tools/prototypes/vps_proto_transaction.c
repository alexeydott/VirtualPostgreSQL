#include "vps_proto_common.h"

#include <stdio.h>
#include <string.h>

static const char VPS_SQL_BEGIN[] = "BEGIN";
static const char VPS_SQL_COMMIT[] = "COMMIT";
static const char VPS_SQL_ROLLBACK[] = "ROLLBACK";
static const char VPS_SQL_SAVEPOINT_1[] = "SAVEPOINT vps_s1";
static const char VPS_SQL_SAVEPOINT_2[] = "SAVEPOINT vps_s2";
static const char VPS_SQL_ROLLBACK_TO_1[] = "ROLLBACK TO SAVEPOINT vps_s1";
static const char VPS_SQL_RELEASE_1[] = "RELEASE SAVEPOINT vps_s1";
static const char VPS_SQL_RELEASE_MISSING[] = "RELEASE SAVEPOINT vps_missing";
static const char VPS_SQL_ERROR[] = "SELECT 1 / 0";
static const char VPS_SQL_VALUE[] = "SELECT 1::int4";
static const char VPS_SQL_STREAM[] = "SELECT g::int4 FROM generate_series(1, 100) AS g";

typedef struct vps_transaction_context {
    unsigned int results;
    unsigned int clears;
} vps_transaction_context;

static const char *vps_transaction_name(PGTransactionStatusType status)
{
    switch (status) {
        case PQTRANS_IDLE: return "idle";
        case PQTRANS_ACTIVE: return "active";
        case PQTRANS_INTRANS: return "in_transaction";
        case PQTRANS_INERROR: return "in_error";
        case PQTRANS_UNKNOWN: return "unknown";
        default: return "invalid";
    }
}

static int vps_command(PGconn *connection,
                       const char *operation,
                       const char *sql,
                       ExecStatusType expected_status,
                       PGTransactionStatusType expected_transaction,
                       vps_transaction_context *context)
{
    vps_deadline deadline = vps_deadline_after(10000U);
    PGTransactionStatusType old_status = PQtransactionStatus(connection);
    int operation_ok = 0;
    PGresult *result;
    ExecStatusType status;
    char sqlstate[6];
    char fields[192];
    int ok;

    if (PQsendQueryParams(connection, sql, 0, NULL, NULL, NULL, NULL, 0) == 0 ||
        !vps_flush_async(connection, &deadline, NULL)) {
        return 0;
    }
    result = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
    if (!operation_ok || result == NULL) {
        if (result != NULL) { PQclear(result); }
        return 0;
    }
    context->results++;
    status = PQresultStatus(result);
    (void)snprintf(sqlstate, sizeof(sqlstate), "%s", vps_sqlstate(result));
    PQclear(result);
    context->clears++;
    result = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
    if (!operation_ok || result != NULL) {
        if (result != NULL) { PQclear(result); context->clears++; context->results++; }
        return 0;
    }
    (void)snprintf(fields, sizeof(fields), "operation=%s old=%s new=%s result=%s sqlstate=%s",
                   operation, vps_transaction_name(old_status), vps_transaction_name(PQtransactionStatus(connection)),
                   vps_result_status_name(status), sqlstate);
    vps_log(status == expected_status ? "info" : "error", "transaction_transition", fields);
    ok = status == expected_status && PQtransactionStatus(connection) == expected_transaction;
    return ok;
}

static int vps_normal_case(int commit)
{
    vps_transaction_context context = { 0U, 0U };
    PGconn *connection = vps_connect_test_server(5000U);
    int ok;
    if (connection == NULL) { return 0; }
    ok = vps_command(connection, "begin", VPS_SQL_BEGIN, PGRES_COMMAND_OK, PQTRANS_INTRANS, &context) &&
         vps_command(connection, "value", VPS_SQL_VALUE, PGRES_TUPLES_OK, PQTRANS_INTRANS, &context) &&
         vps_command(connection, commit ? "commit" : "rollback", commit ? VPS_SQL_COMMIT : VPS_SQL_ROLLBACK,
                     PGRES_COMMAND_OK, PQTRANS_IDLE, &context);
    PQfinish(connection);
    return ok && context.results == context.clears;
}

static int vps_savepoint_case(void)
{
    vps_transaction_context context = { 0U, 0U };
    PGconn *connection = vps_connect_test_server(5000U);
    int ok;
    if (connection == NULL) { return 0; }
    ok = vps_command(connection, "begin", VPS_SQL_BEGIN, PGRES_COMMAND_OK, PQTRANS_INTRANS, &context) &&
         vps_command(connection, "savepoint", VPS_SQL_SAVEPOINT_1, PGRES_COMMAND_OK, PQTRANS_INTRANS, &context) &&
         vps_command(connection, "constraint_error", VPS_SQL_ERROR, PGRES_FATAL_ERROR, PQTRANS_INERROR, &context) &&
         vps_command(connection, "rollback_to", VPS_SQL_ROLLBACK_TO_1, PGRES_COMMAND_OK, PQTRANS_INTRANS, &context) &&
         vps_command(connection, "release", VPS_SQL_RELEASE_1, PGRES_COMMAND_OK, PQTRANS_INTRANS, &context) &&
         vps_command(connection, "commit", VPS_SQL_COMMIT, PGRES_COMMAND_OK, PQTRANS_IDLE, &context);
    PQfinish(connection);
    return ok && context.results == context.clears;
}

static int vps_invalid_release_case(void)
{
    vps_transaction_context context = { 0U, 0U };
    PGconn *connection = vps_connect_test_server(5000U);
    int ok;
    if (connection == NULL) { return 0; }
    ok = vps_command(connection, "begin", VPS_SQL_BEGIN, PGRES_COMMAND_OK, PQTRANS_INTRANS, &context) &&
         vps_command(connection, "savepoint_1", VPS_SQL_SAVEPOINT_1, PGRES_COMMAND_OK, PQTRANS_INTRANS, &context) &&
         vps_command(connection, "savepoint_2", VPS_SQL_SAVEPOINT_2, PGRES_COMMAND_OK, PQTRANS_INTRANS, &context) &&
         vps_command(connection, "invalid_release", VPS_SQL_RELEASE_MISSING, PGRES_FATAL_ERROR, PQTRANS_INERROR, &context) &&
         vps_command(connection, "rollback", VPS_SQL_ROLLBACK, PGRES_COMMAND_OK, PQTRANS_IDLE, &context);
    PQfinish(connection);
    return ok && context.results == context.clears;
}

static int vps_outside_error_case(void)
{
    vps_transaction_context context = { 0U, 0U };
    PGconn *connection = vps_connect_test_server(5000U);
    int ok;
    if (connection == NULL) { return 0; }
    ok = vps_command(connection, "outside_error", VPS_SQL_ERROR, PGRES_FATAL_ERROR, PQTRANS_IDLE, &context) &&
         vps_command(connection, "post_error_value", VPS_SQL_VALUE, PGRES_TUPLES_OK, PQTRANS_IDLE, &context);
    PQfinish(connection);
    return ok && context.results == context.clears;
}

static int vps_active_stream_case(void)
{
    PGconn *connection = vps_connect_test_server(5000U);
    vps_deadline deadline = vps_deadline_after(10000U);
    unsigned int results = 0U;
    unsigned int clears = 0U;
    unsigned int rows = 0U;
    int operation_ok = 0;
    int conflict_rejected;
    int terminal_ok = 0;

    if (connection == NULL) { return 0; }
    if (PQsendQueryParams(connection, VPS_SQL_STREAM, 0, NULL, NULL, NULL, NULL, 1) == 0 ||
        PQsetSingleRowMode(connection) == 0 || !vps_flush_async(connection, &deadline, NULL)) {
        PQfinish(connection);
        return 0;
    }
    {
        PGresult *first = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
        if (!operation_ok || first == NULL || PQresultStatus(first) != PGRES_SINGLE_TUPLE) {
            if (first != NULL) { PQclear(first); }
            PQfinish(connection);
            return 0;
        }
        results++;
        rows++;
        PQclear(first);
        clears++;
    }
    conflict_rejected = PQsendQueryParams(connection, VPS_SQL_VALUE, 0, NULL, NULL, NULL, NULL, 0) == 0;
    for (;;) {
        PGresult *result = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
        if (!operation_ok) { break; }
        if (result == NULL) { break; }
        results++;
        if (PQresultStatus(result) == PGRES_SINGLE_TUPLE) { rows++; }
        if (PQresultStatus(result) == PGRES_TUPLES_OK) { terminal_ok = 1; }
        PQclear(result);
        clears++;
    }
    vps_log(conflict_rejected ? "info" : "error", "active_command_conflict",
            "operation=second_send outcome=rejected pending_stream=true");
    PQfinish(connection);
    return operation_ok && conflict_rejected && terminal_ok && rows == 100U && results == clears;
}

static int vps_loss_case(int during_terminal)
{
    vps_transaction_context context = { 0U, 0U };
    PGconn *connection = vps_connect_test_server(5000U);
    int socket_value;
    int send_accepted = 0;
    int ambiguous = 0;
    char fields[128];

    if (connection == NULL) { return 0; }
    if (!vps_command(connection, "begin", VPS_SQL_BEGIN, PGRES_COMMAND_OK, PQTRANS_INTRANS, &context)) {
        PQfinish(connection);
        return 0;
    }
    socket_value = PQsocket(connection);
    if (during_terminal) {
        send_accepted = PQsendQueryParams(connection, VPS_SQL_COMMIT, 0, NULL, NULL, NULL, NULL, 0);
        if (send_accepted != 0) {
            (void)PQflush(connection);
            ambiguous = 1;
        }
    }
    if (socket_value >= 0) {
        (void)shutdown((SOCKET)(uintptr_t)(unsigned int)socket_value, SD_BOTH);
    }
    if (!during_terminal) {
        send_accepted = PQsendQueryParams(connection, VPS_SQL_COMMIT, 0, NULL, NULL, NULL, NULL, 0);
        ambiguous = 0;
    }
    (void)snprintf(fields, sizeof(fields), "phase=%s send_accepted=%s ambiguous=%s decision=destroy retry=false",
                   during_terminal ? "during_terminal" : "before_terminal",
                   send_accepted ? "true" : "false", ambiguous ? "true" : "false");
    vps_log("info", "transaction_connection_loss", fields);
    PQfinish(connection);
    return during_terminal ? ambiguous : !ambiguous;
}

int main(int argc, char **argv)
{
    int ok = 0;
    if (argc != 2) {
        vps_log("error", "usage", "modes=commit,rollback,savepoint,outside-error,invalid-release,active-stream,loss-before-terminal,loss-during-terminal");
        return 64;
    }
    if (strcmp(argv[1], "commit") == 0) { ok = vps_normal_case(1); }
    else if (strcmp(argv[1], "rollback") == 0) { ok = vps_normal_case(0); }
    else if (strcmp(argv[1], "savepoint") == 0) { ok = vps_savepoint_case(); }
    else if (strcmp(argv[1], "outside-error") == 0) { ok = vps_outside_error_case(); }
    else if (strcmp(argv[1], "invalid-release") == 0) { ok = vps_invalid_release_case(); }
    else if (strcmp(argv[1], "active-stream") == 0) { ok = vps_active_stream_case(); }
    else if (strcmp(argv[1], "loss-before-terminal") == 0) { ok = vps_loss_case(0); }
    else if (strcmp(argv[1], "loss-during-terminal") == 0) { ok = vps_loss_case(1); }
    else { return 64; }
    vps_log(ok ? "info" : "error", "transaction_case_complete", ok ? "outcome=pass" : "outcome=fail");
    return ok ? 0 : 1;
}
