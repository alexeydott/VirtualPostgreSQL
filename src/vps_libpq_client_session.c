#include "vps_libpq_client_session.h"

#include <libpq-fe.h>

#include <string.h>

typedef struct VpsLibpqSessionContext {
    PGconn *connection;
} VpsLibpqSessionContext;

static VpsSessionResult vps_libpq_session_inspect(
    void *context,
    VpsSessionConnectionState *state)
{
    VpsLibpqSessionContext *session = (VpsLibpqSessionContext *)context;
    PGresult *pending;
    if (session == NULL || session->connection == NULL || state == NULL ||
        PQstatus(session->connection) != CONNECTION_OK) {
        return VPS_SESSION_CLIENT_ERROR;
    }
    state->transaction_idle =
        PQtransactionStatus(session->connection) == PQTRANS_IDLE;
    state->pipeline_disabled =
        PQpipelineStatus(session->connection) == PQ_PIPELINE_OFF;
    if (PQisBusy(session->connection)) {
        state->pending_results_absent = 0;
        return VPS_SESSION_OK;
    }
    pending = PQgetResult(session->connection);
    if (pending == NULL) {
        state->pending_results_absent = 1;
        return VPS_SESSION_OK;
    }
    do {
        PQclear(pending);
        pending = PQgetResult(session->connection);
    } while (pending != NULL);
    state->pending_results_absent = 0;
    return VPS_SESSION_OK;
}

static VpsSessionResult vps_libpq_session_apply_setting(
    void *context,
    const VpsSessionPlan *plan,
    const VpsSessionSetting *setting)
{
    static const char query[] =
        "SELECT pg_catalog.set_config($1, $2, false)";
    VpsLibpqSessionContext *session = (VpsLibpqSessionContext *)context;
    const char *parameter;
    const char *source_value;
    const char *values[2];
    char value[VPS_SESSION_SEARCH_PATH_LIMIT + 1U];
    PGresult *result;
    VpsSessionResult session_result = VPS_SESSION_CLIENT_ERROR;
    if (session == NULL || session->connection == NULL || plan == NULL ||
        setting == NULL || setting->value_length >= sizeof(value)) {
        return VPS_SESSION_INVALID_ARGUMENT;
    }
    parameter = vps_session_parameter_name(setting->parameter);
    source_value = vps_session_setting_value(plan, setting);
    if (source_value == NULL || strcmp(parameter, "unknown") == 0) {
        return VPS_SESSION_INVALID_ARGUMENT;
    }
    if (setting->value_length != 0U) {
        (void)memcpy(value, source_value, setting->value_length);
    }
    value[setting->value_length] = '\0';
    values[0] = parameter;
    values[1] = value;
    result = PQexecParams(session->connection, query, 2, NULL, values, NULL,
                          NULL, 0);
    (void)memset(value, 0, sizeof(value));
    if (result == NULL) return VPS_SESSION_CLIENT_ERROR;
    if (PQresultStatus(result) == PGRES_TUPLES_OK &&
        PQntuples(result) == 1 && PQnfields(result) == 1 &&
        !PQgetisnull(result, 0, 0)) {
        const char *observed = PQgetvalue(result, 0, 0);
        int observed_length = PQgetlength(result, 0, 0);
        session_result = observed_length >= 0 &&
                                 vps_session_setting_matches(
                                     plan, setting, observed,
                                     (size_t)observed_length)
                             ? VPS_SESSION_OK
                             : VPS_SESSION_OBSERVED_MISMATCH;
    }
    PQclear(result);
    return session_result;
}

VpsSessionResult vps_libpq_client_session_apply(void *connection,
                                                 const VpsSessionPlan *plan,
                                                 VpsSessionPhase phase)
{
    VpsLibpqSessionContext context;
    VpsSessionClientOperations operations;
    if (connection == NULL || plan == NULL) return VPS_SESSION_INVALID_ARGUMENT;
    context.connection = (PGconn *)connection;
    operations.context = &context;
    operations.apply_setting = vps_libpq_session_apply_setting;
    operations.inspect = vps_libpq_session_inspect;
    return vps_session_apply(plan, &operations, phase);
}
