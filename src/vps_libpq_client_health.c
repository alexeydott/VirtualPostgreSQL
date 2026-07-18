#include "vps_libpq_client_internal.h"

#include <string.h>

static const char vps_health_set_config_query[] =
    "SELECT pg_catalog.set_config($1, $2, false)";
static const char vps_health_ping_query[] = "SELECT 1";
static const char vps_health_discard_query[] = "DISCARD ALL";
static const char vps_health_begin_query[] = "BEGIN";
static const char vps_health_commit_query[] = "COMMIT";
static const char vps_health_rollback_query[] = "ROLLBACK";

static VpsClientStatus vps_health_fail(VpsLibpqConnection *connection,
                                       VpsError *error,
                                       VpsErrorClass error_class)
{
    connection->phase = VPS_LIBPQ_PHASE_FAILED;
    connection->health_phase = VPS_LIBPQ_HEALTH_IDLE;
    return vps_libpq_set_error(error, error_class);
}

static int vps_health_preflight(VpsLibpqConnection *connection,
                                VpsClientOperation operation)
{
    VpsLibpqClient *client = connection->client;
    VpsLibpqTransactionStatus transaction_status;
    void *pending;
    transaction_status = client->api.transaction_status(
        client->api.context, connection->postgresql_connection);
    if (client->api.connection_status(client->api.context,
                                      connection->postgresql_connection) !=
            VPS_LIBPQ_CONNECTION_OK ||
        client->api.pipeline_status(client->api.context,
                                    connection->postgresql_connection) !=
            VPS_LIBPQ_PIPELINE_OFF ||
        connection->statement_count != 0U ||
        client->api.is_busy(client->api.context,
                            connection->postgresql_connection)) {
        return 0;
    }
    if ((operation == VPS_CLIENT_OPERATION_BEGIN ||
         operation == VPS_CLIENT_OPERATION_RESET ||
         operation == VPS_CLIENT_OPERATION_PING) &&
        transaction_status != VPS_LIBPQ_TRANSACTION_IDLE)
        return 0;
    if ((operation == VPS_CLIENT_OPERATION_COMMIT ||
         operation == VPS_CLIENT_OPERATION_ROLLBACK) &&
        transaction_status != VPS_LIBPQ_TRANSACTION_ACTIVE &&
        transaction_status != VPS_LIBPQ_TRANSACTION_INERROR)
        return 0;
    pending = client->api.get_result(client->api.context,
                                     connection->postgresql_connection);
    if (pending != NULL) {
        client->api.clear_result(client->api.context, pending);
        return 0;
    }
    return 1;
}

VpsClientStatus vps_libpq_health_begin(VpsLibpqConnection *connection,
                                       VpsClientOperation operation,
                                       VpsSessionPhase session_phase,
                                       VpsError *error)
{
    if (connection == NULL || connection->client == NULL ||
        (operation != VPS_CLIENT_OPERATION_CONNECT &&
         operation != VPS_CLIENT_OPERATION_RESET &&
         operation != VPS_CLIENT_OPERATION_PING &&
         operation != VPS_CLIENT_OPERATION_BEGIN &&
         operation != VPS_CLIENT_OPERATION_COMMIT &&
         operation != VPS_CLIENT_OPERATION_ROLLBACK)) {
        return VPS_CLIENT_INVALID_ARGUMENT;
    }
    if (operation != VPS_CLIENT_OPERATION_CONNECT &&
        !vps_health_preflight(connection, operation)) {
        return vps_health_fail(connection, error,
                               VPS_ERROR_CLASS_CONNECTION);
    }
    connection->active_operation = operation;
    connection->session_phase = session_phase;
    connection->session_index = 0U;
    connection->health_result_seen = 0;
    connection->reset_discard_complete = 0;
    connection->health_phase = VPS_LIBPQ_HEALTH_SEND;
    connection->phase = VPS_LIBPQ_PHASE_HEALTH;
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_health_send(VpsLibpqConnection *connection,
                                       VpsError *error)
{
    VpsLibpqClient *client = connection->client;
    const char *query;
    const char *values[2];
    int parameter_count = 0;
    int sent;

    if (connection->active_operation == VPS_CLIENT_OPERATION_BEGIN) {
        query = vps_health_begin_query;
    } else if (connection->active_operation == VPS_CLIENT_OPERATION_COMMIT) {
        query = vps_health_commit_query;
    } else if (connection->active_operation == VPS_CLIENT_OPERATION_ROLLBACK) {
        query = vps_health_rollback_query;
    } else if (connection->active_operation == VPS_CLIENT_OPERATION_RESET &&
        client->reset_mode == VPS_LIBPQ_RESET_DISCARD_ALL &&
        !connection->reset_discard_complete) {
        query = vps_health_discard_query;
    } else if (connection->active_operation == VPS_CLIENT_OPERATION_PING) {
        query = vps_health_ping_query;
    } else {
        const VpsSessionSetting *setting = vps_session_setting_at(
            client->session_plan, connection->session_index);
        const char *value;
        const char *parameter;
        if (setting == NULL) {
            connection->phase = VPS_LIBPQ_PHASE_READY;
            connection->health_phase = VPS_LIBPQ_HEALTH_IDLE;
            connection->active_operation = VPS_CLIENT_OPERATION_NONE;
            return VPS_CLIENT_OK;
        }
        value = vps_session_setting_value(client->session_plan, setting);
        parameter = vps_session_parameter_name(setting->parameter);
        if (value == NULL || setting->value_length >=
                                 sizeof(connection->session_value)) {
            return vps_health_fail(connection, error,
                                   VPS_ERROR_CLASS_CONFIG);
        }
        if (setting->value_length != 0U) {
            (void)memcpy(connection->session_value, value,
                         setting->value_length);
        }
        connection->session_value[setting->value_length] = '\0';
        values[0] = parameter;
        values[1] = connection->session_value;
        parameter_count = 2;
        query = vps_health_set_config_query;
    }
    sent = client->api.send_query_params(
        client->api.context, connection->postgresql_connection, query,
        parameter_count, NULL, parameter_count == 0 ? NULL : values, NULL,
        NULL, 0);
    if (sent != 1) {
        return vps_health_fail(connection, error,
                               VPS_ERROR_CLASS_CONNECTION);
    }
    connection->health_result_seen = 0;
    connection->health_phase = VPS_LIBPQ_HEALTH_FLUSH;
    return VPS_CLIENT_OK;
}

static int vps_health_result_valid(VpsLibpqConnection *connection,
                                   const void *result)
{
    VpsLibpqClient *client = connection->client;
    VpsLibpqResultStatus status = client->api.result_status(
        client->api.context, result);
    if (connection->active_operation == VPS_CLIENT_OPERATION_BEGIN ||
        connection->active_operation == VPS_CLIENT_OPERATION_COMMIT ||
        connection->active_operation == VPS_CLIENT_OPERATION_ROLLBACK)
        return status == VPS_LIBPQ_RESULT_COMMAND_OK;
    if (connection->active_operation == VPS_CLIENT_OPERATION_RESET &&
        client->reset_mode == VPS_LIBPQ_RESET_DISCARD_ALL &&
        !connection->reset_discard_complete) {
        return status == VPS_LIBPQ_RESULT_COMMAND_OK;
    }
    if (status != VPS_LIBPQ_RESULT_TUPLES_OK ||
        client->api.result_row_count(client->api.context, result) != 1 ||
        client->api.result_field_count(client->api.context, result) != 1 ||
        client->api.result_value_is_null(client->api.context, result, 0, 0)) {
        return 0;
    }
    if (connection->active_operation == VPS_CLIENT_OPERATION_PING) {
        return 1;
    }
    {
        const VpsSessionSetting *setting = vps_session_setting_at(
            client->session_plan, connection->session_index);
        int length = client->api.result_value_length(client->api.context,
                                                     result, 0, 0);
        const char *value = (const char *)client->api.result_value(
            client->api.context, result, 0, 0);
        return setting != NULL && length >= 0 && value != NULL &&
               vps_session_setting_matches(client->session_plan, setting,
                                           value, (size_t)length);
    }
}

static void vps_health_command_complete(VpsLibpqConnection *connection)
{
    VpsLibpqClient *client = connection->client;
    if (connection->active_operation == VPS_CLIENT_OPERATION_BEGIN ||
        connection->active_operation == VPS_CLIENT_OPERATION_COMMIT ||
        connection->active_operation == VPS_CLIENT_OPERATION_ROLLBACK) {
        connection->phase = VPS_LIBPQ_PHASE_READY;
        connection->health_phase = VPS_LIBPQ_HEALTH_IDLE;
        connection->active_operation = VPS_CLIENT_OPERATION_NONE;
        return;
    }
    if (connection->active_operation == VPS_CLIENT_OPERATION_PING) {
        connection->phase = VPS_LIBPQ_PHASE_READY;
        connection->health_phase = VPS_LIBPQ_HEALTH_IDLE;
        connection->active_operation = VPS_CLIENT_OPERATION_NONE;
        return;
    }
    if (connection->active_operation == VPS_CLIENT_OPERATION_RESET &&
        client->reset_mode == VPS_LIBPQ_RESET_DISCARD_ALL &&
        !connection->reset_discard_complete) {
        connection->reset_discard_complete = 1;
    } else {
        connection->session_index += 1U;
    }
    connection->health_phase = VPS_LIBPQ_HEALTH_SEND;
}

VpsClientStatus vps_libpq_health_poll(VpsLibpqConnection *connection,
                                      VpsClientPollResult *result,
                                      VpsError *error)
{
    VpsLibpqClient *client;
    if (connection == NULL || result == NULL ||
        connection->phase != VPS_LIBPQ_PHASE_HEALTH) {
        return VPS_CLIENT_INVALID_STATE;
    }
    client = connection->client;
    for (;;) {
        if (connection->health_phase == VPS_LIBPQ_HEALTH_SEND) {
            VpsClientStatus send_result = vps_health_send(connection, error);
            if (send_result != VPS_CLIENT_OK) {
                return send_result;
            }
            if (connection->phase == VPS_LIBPQ_PHASE_READY) {
                (void)memset(result, 0, sizeof(*result));
                result->outcome = VPS_CLIENT_POLL_COMPLETE;
                return VPS_CLIENT_OK;
            }
        }
        if (connection->health_phase == VPS_LIBPQ_HEALTH_FLUSH) {
            int flush = client->api.flush(client->api.context,
                                          connection->postgresql_connection);
            if (flush < 0) {
                return vps_health_fail(connection, error,
                                       VPS_ERROR_CLASS_CONNECTION);
            }
            if (flush == 1) {
                connection->wait_interest = VPS_CLIENT_WAIT_WRITE;
                break;
            }
            connection->health_phase = VPS_LIBPQ_HEALTH_RECEIVE;
        }
        if (connection->health_phase == VPS_LIBPQ_HEALTH_RECEIVE) {
            void *postgresql_result;
            if (!client->api.consume_input(client->api.context,
                                           connection->postgresql_connection)) {
                return vps_health_fail(connection, error,
                                       VPS_ERROR_CLASS_CONNECTION);
            }
            if (client->api.is_busy(client->api.context,
                                    connection->postgresql_connection)) {
                connection->wait_interest = VPS_CLIENT_WAIT_READ;
                break;
            }
            postgresql_result = client->api.get_result(
                client->api.context, connection->postgresql_connection);
            if (postgresql_result == NULL) {
                if (!connection->health_result_seen) {
                    return vps_health_fail(connection, error,
                                           VPS_ERROR_CLASS_CONNECTION);
                }
                vps_health_command_complete(connection);
                if (connection->phase == VPS_LIBPQ_PHASE_READY) {
                    (void)memset(result, 0, sizeof(*result));
                    result->outcome = VPS_CLIENT_POLL_COMPLETE;
                    return VPS_CLIENT_OK;
                }
                continue;
            }
            if (connection->health_result_seen ||
                !vps_health_result_valid(connection, postgresql_result)) {
                const char *sqlstate = client->api.result_sqlstate(
                    client->api.context, postgresql_result);
                if (sqlstate != NULL) {
                    VpsErrorOperation error_operation =
                        connection->active_operation == VPS_CLIENT_OPERATION_COMMIT
                            ? VPS_ERROR_OPERATION_COMMIT
                            : connection->active_operation == VPS_CLIENT_OPERATION_ROLLBACK
                                  ? VPS_ERROR_OPERATION_ROLLBACK
                                  : VPS_ERROR_OPERATION_CONNECT;
                    (void)vps_error_set_sqlstate(error,
                                                 error_operation,
                                                 sqlstate, 0, 0, NULL);
                }
                client->api.clear_result(client->api.context,
                                         postgresql_result);
                return vps_health_fail(
                    connection, error,
                    connection->active_operation == VPS_CLIENT_OPERATION_PING
                        ? VPS_ERROR_CLASS_CONNECTION
                        : VPS_ERROR_CLASS_CONFIG);
            }
            client->api.clear_result(client->api.context, postgresql_result);
            connection->health_result_seen = 1;
            continue;
        }
    }
    connection->socket_handle = client->api.socket_handle(
        client->api.context, connection->postgresql_connection);
    if (connection->socket_handle < 0) {
        return vps_health_fail(connection, error,
                               VPS_ERROR_CLASS_CONNECTION);
    }
    (void)memset(result, 0, sizeof(*result));
    result->outcome = VPS_CLIENT_POLL_WAIT;
    result->wait.interest = connection->wait_interest;
    result->wait.phase = connection->active_operation ==
                                 VPS_CLIENT_OPERATION_RESET
                             ? VPS_CLIENT_WAIT_RESET
                             : connection->active_operation ==
                                       VPS_CLIENT_OPERATION_PING
                                   ? VPS_CLIENT_WAIT_PING
                                   : connection->active_operation ==
                                             VPS_CLIENT_OPERATION_BEGIN ||
                                         connection->active_operation ==
                                             VPS_CLIENT_OPERATION_COMMIT ||
                                         connection->active_operation ==
                                             VPS_CLIENT_OPERATION_ROLLBACK
                                       ? VPS_CLIENT_WAIT_TRANSACTION
                                       : VPS_CLIENT_WAIT_CONNECT;
    result->wait.max_slice_ms = client->wait_slice_ms;
    return VPS_CLIENT_OK;
}
