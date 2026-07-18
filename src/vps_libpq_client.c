#include "vps_libpq_client.h"
#include "vps_libpq_client_internal.h"

#include "vps_libpq_client_tls.h"

#include <libpq-fe.h>

#include <stddef.h>
#include <string.h>

typedef struct VpsLibpqKeywordField {
    const char *keyword;
    uint64_t field;
    size_t offset;
} VpsLibpqKeywordField;

#define VPS_LIBPQ_CONFIG_OFFSET(member) offsetof(VpsCredentialConfig, member)

static const VpsLibpqKeywordField vps_libpq_keyword_fields[] = {
    {"host", VPS_CREDENTIAL_FIELD_HOSTS, VPS_LIBPQ_CONFIG_OFFSET(hosts)},
    {"port", VPS_CREDENTIAL_FIELD_PORTS, VPS_LIBPQ_CONFIG_OFFSET(ports)},
    {"user", VPS_CREDENTIAL_FIELD_USER, VPS_LIBPQ_CONFIG_OFFSET(user)},
    {"password", VPS_CREDENTIAL_FIELD_PASSWORD,
     VPS_LIBPQ_CONFIG_OFFSET(password)},
    {"dbname", VPS_CREDENTIAL_FIELD_DBNAME, VPS_LIBPQ_CONFIG_OFFSET(dbname)},
    {"service", VPS_CREDENTIAL_FIELD_SERVICE,
     VPS_LIBPQ_CONFIG_OFFSET(service)},
    {"servicefile", VPS_CREDENTIAL_FIELD_SERVICE_FILE,
     VPS_LIBPQ_CONFIG_OFFSET(service_file)},
    {"sslrootcert", VPS_CREDENTIAL_FIELD_SSLROOTCERT,
     VPS_LIBPQ_CONFIG_OFFSET(sslrootcert)},
    {"sslcert", VPS_CREDENTIAL_FIELD_SSLCERT,
     VPS_LIBPQ_CONFIG_OFFSET(sslcert)},
    {"sslkey", VPS_CREDENTIAL_FIELD_SSLKEY,
     VPS_LIBPQ_CONFIG_OFFSET(sslkey)},
    {"sslcrl", VPS_CREDENTIAL_FIELD_SSLCRL,
     VPS_LIBPQ_CONFIG_OFFSET(sslcrl)},
    {"target_session_attrs", VPS_CREDENTIAL_FIELD_TARGET_SESSION_ATTRS,
     VPS_LIBPQ_CONFIG_OFFSET(target_session_attrs)},
    {"connect_timeout", VPS_CREDENTIAL_FIELD_CONNECT_TIMEOUT,
     VPS_LIBPQ_CONFIG_OFFSET(connect_timeout)},
    {"application_name", VPS_CREDENTIAL_FIELD_APPLICATION_NAME,
     VPS_LIBPQ_CONFIG_OFFSET(application_name)}};

static void *vps_libpq_default_connect_start(
    void *context,
    const char *const *keywords,
    const char *const *values,
    int expand_dbname)
{
    (void)context;
    return PQconnectStartParams(keywords, values, expand_dbname);
}

static int vps_libpq_default_set_nonblocking(void *context,
                                              void *connection,
                                              int enabled)
{
    (void)context;
    return PQsetnonblocking((PGconn *)connection, enabled);
}

static VpsLibpqPollingStatus vps_libpq_default_connect_poll(
    void *context,
    void *connection)
{
    PostgresPollingStatusType status;
    (void)context;
    status = PQconnectPoll((PGconn *)connection);
    switch (status) {
    case PGRES_POLLING_FAILED: return VPS_LIBPQ_POLL_FAILED;
    case PGRES_POLLING_READING: return VPS_LIBPQ_POLL_READING;
    case PGRES_POLLING_WRITING: return VPS_LIBPQ_POLL_WRITING;
    case PGRES_POLLING_OK: return VPS_LIBPQ_POLL_OK;
    case PGRES_POLLING_ACTIVE: return VPS_LIBPQ_POLL_ACTIVE;
    default: return VPS_LIBPQ_POLL_FAILED;
    }
}

static intptr_t vps_libpq_default_socket_handle(void *context,
                                                 const void *connection)
{
    int socket_value;
    (void)context;
    socket_value = PQsocket((const PGconn *)connection);
    return socket_value < 0
               ? (intptr_t)-1
               : (intptr_t)(uintptr_t)(unsigned int)socket_value;
}

static VpsLibpqConnectionStatus vps_libpq_default_connection_status(
    void *context,
    const void *connection)
{
    (void)context;
    return PQstatus((const PGconn *)connection) == CONNECTION_OK
               ? VPS_LIBPQ_CONNECTION_OK
               : VPS_LIBPQ_CONNECTION_BAD;
}

static VpsLibpqTransactionStatus vps_libpq_default_transaction_status(
    void *context, const void *connection)
{
    PGTransactionStatusType status;
    (void)context;
    status = PQtransactionStatus((const PGconn *)connection);
    return status == PQTRANS_IDLE
               ? VPS_LIBPQ_TRANSACTION_IDLE
               : status == PQTRANS_ACTIVE || status == PQTRANS_INTRANS ||
                         status == PQTRANS_INERROR
                     ? VPS_LIBPQ_TRANSACTION_ACTIVE
                     : VPS_LIBPQ_TRANSACTION_UNKNOWN;
}

static VpsLibpqPipelineStatus vps_libpq_default_pipeline_status(
    void *context, const void *connection)
{
    (void)context;
    return PQpipelineStatus((const PGconn *)connection) == PQ_PIPELINE_OFF
               ? VPS_LIBPQ_PIPELINE_OFF
               : VPS_LIBPQ_PIPELINE_ON;
}

static char vps_libpq_ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int vps_libpq_message_contains(const char *message,
                                      const char *fragment)
{
    size_t fragment_length;
    size_t index;
    if (message == NULL || fragment == NULL) return 0;
    fragment_length = strlen(fragment);
    if (fragment_length == 0U) return 1;
    for (; *message != '\0'; ++message) {
        for (index = 0U; index < fragment_length; ++index) {
            if (message[index] == '\0' ||
                vps_libpq_ascii_lower(message[index]) != fragment[index]) {
                break;
            }
        }
        if (index == fragment_length) return 1;
    }
    return 0;
}

static VpsErrorClass vps_libpq_default_error_class(
    void *context,
    const void *connection)
{
    const PGconn *postgresql_connection = (const PGconn *)connection;
    const char *message;
    (void)context;
    if (postgresql_connection == NULL) return VPS_ERROR_CLASS_CONNECTION;
    if (PQconnectionNeedsPassword(postgresql_connection)) {
        return VPS_ERROR_CLASS_AUTH;
    }
    message = PQerrorMessage(postgresql_connection);
    if (vps_libpq_message_contains(message, "password authentication") ||
        vps_libpq_message_contains(message, "authentication failed") ||
        vps_libpq_message_contains(message, "no password supplied") ||
        vps_libpq_message_contains(message, "no pg_hba.conf entry")) {
        return VPS_ERROR_CLASS_AUTH;
    }
    if (vps_libpq_message_contains(message, "certificate") ||
        vps_libpq_message_contains(message, "ssl") ||
        vps_libpq_message_contains(message, "tls") ||
        vps_libpq_message_contains(message, "channel binding")) {
        return VPS_ERROR_CLASS_TLS;
    }
    if (vps_libpq_message_contains(message, "invalid connection option") ||
        vps_libpq_message_contains(message, "invalid sslmode") ||
        vps_libpq_message_contains(message, "definition of service")) {
        return VPS_ERROR_CLASS_CONFIG;
    }
    return VPS_ERROR_CLASS_CONNECTION;
}

static VpsTlsResult vps_libpq_default_tls_verify(
    void *context,
    void *connection,
    const VpsTlsPolicy *policy,
    VpsTlsOutcome *outcome,
    VpsLogger *logger)
{
    (void)context;
    return vps_libpq_client_tls_verify(connection, policy, outcome, logger);
}

static int vps_libpq_default_identity_verify(
    void *context,
    const void *connection,
    const VpsConnectionIdentity *identity)
{
    const char *fingerprint;
    size_t index;
    (void)context;
    if (connection == NULL || identity == NULL || !identity->initialized ||
        !identity->built) return 0;
    fingerprint = vps_identity_fingerprint(identity);
    if (fingerprint == NULL) return 0;
    for (index = 0U; index < VPS_IDENTITY_FINGERPRINT_LENGTH; ++index) {
        char value = fingerprint[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) return 0;
    }
    return fingerprint[VPS_IDENTITY_FINGERPRINT_LENGTH] == '\0';
}

static int vps_libpq_default_send_prepare(
    void *context, void *connection, const char *name, const char *query,
    int parameter_count, const uint32_t *parameter_types)
{
    (void)context;
    return PQsendPrepare((PGconn *)connection, name, query, parameter_count,
                         (const Oid *)parameter_types);
}

static int vps_libpq_default_send_describe_prepared(
    void *context, void *connection, const char *name)
{
    (void)context;
    return PQsendDescribePrepared((PGconn *)connection, name);
}

static int vps_libpq_default_send_query_prepared(
    void *context, void *connection, const char *name, int parameter_count,
    const char *const *values, const int *lengths, const int *formats,
    int result_format)
{
    (void)context;
    return PQsendQueryPrepared((PGconn *)connection, name, parameter_count,
                               values, lengths, formats, result_format);
}

static int vps_libpq_default_send_query_params(
    void *context, void *connection, const char *query, int parameter_count,
    const uint32_t *parameter_types, const char *const *values,
    const int *lengths, const int *formats, int result_format)
{
    (void)context;
    return PQsendQueryParams((PGconn *)connection, query, parameter_count,
                             (const Oid *)parameter_types, values, lengths,
                             formats, result_format);
}

static int vps_libpq_default_set_single_row_mode(void *context,
                                                  void *connection)
{
    (void)context;
    return PQsetSingleRowMode((PGconn *)connection);
}

static void *vps_libpq_default_cancel_create(void *context, void *connection)
{
    (void)context;
    return PQcancelCreate((PGconn *)connection);
}

static int vps_libpq_default_cancel_start(void *context,
                                          void *cancel_connection)
{
    (void)context;
    return PQcancelStart((PGcancelConn *)cancel_connection);
}

static VpsLibpqPollingStatus vps_libpq_default_cancel_poll(
    void *context, void *cancel_connection)
{
    PostgresPollingStatusType status;
    (void)context;
    status = PQcancelPoll((PGcancelConn *)cancel_connection);
    switch (status) {
    case PGRES_POLLING_FAILED: return VPS_LIBPQ_POLL_FAILED;
    case PGRES_POLLING_READING: return VPS_LIBPQ_POLL_READING;
    case PGRES_POLLING_WRITING: return VPS_LIBPQ_POLL_WRITING;
    case PGRES_POLLING_OK: return VPS_LIBPQ_POLL_OK;
    case PGRES_POLLING_ACTIVE: return VPS_LIBPQ_POLL_ACTIVE;
    default: return VPS_LIBPQ_POLL_FAILED;
    }
}

static intptr_t vps_libpq_default_cancel_socket(
    void *context, const void *cancel_connection)
{
    int socket_value;
    (void)context;
    socket_value = PQcancelSocket((const PGcancelConn *)cancel_connection);
    return socket_value < 0 ? (intptr_t)-1
                            : (intptr_t)(uintptr_t)(unsigned int)socket_value;
}

static void vps_libpq_default_cancel_reset(void *context,
                                           void *cancel_connection)
{
    (void)context;
    PQcancelReset((PGcancelConn *)cancel_connection);
}

static void vps_libpq_default_cancel_finish(void *context,
                                            void *cancel_connection)
{
    (void)context;
    PQcancelFinish((PGcancelConn *)cancel_connection);
}

static int vps_libpq_default_flush(void *context, void *connection)
{
    (void)context;
    return PQflush((PGconn *)connection);
}

static int vps_libpq_default_consume_input(void *context, void *connection)
{
    (void)context;
    return PQconsumeInput((PGconn *)connection);
}

static int vps_libpq_default_is_busy(void *context, const void *connection)
{
    (void)context;
    return PQisBusy((PGconn *)connection);
}

static void *vps_libpq_default_get_result(void *context, void *connection)
{
    (void)context;
    return PQgetResult((PGconn *)connection);
}

static VpsLibpqResultStatus vps_libpq_default_result_status(
    void *context, const void *result)
{
    ExecStatusType status;
    (void)context;
    if (result == NULL) return VPS_LIBPQ_RESULT_EMPTY;
    status = PQresultStatus((const PGresult *)result);
    switch (status) {
    case PGRES_COMMAND_OK: return VPS_LIBPQ_RESULT_COMMAND_OK;
    case PGRES_TUPLES_OK: return VPS_LIBPQ_RESULT_TUPLES_OK;
    case PGRES_SINGLE_TUPLE: return VPS_LIBPQ_RESULT_SINGLE_TUPLE;
    case PGRES_FATAL_ERROR: return VPS_LIBPQ_RESULT_FATAL_ERROR;
    default: return VPS_LIBPQ_RESULT_OTHER;
    }
}

static int vps_libpq_default_result_parameter_count(void *context,
                                                     const void *result)
{
    (void)context;
    return PQnparams((const PGresult *)result);
}

static uint32_t vps_libpq_default_result_parameter_type(
    void *context, const void *result, int index)
{
    (void)context;
    return (uint32_t)PQparamtype((const PGresult *)result, index);
}

static int vps_libpq_default_result_field_count(void *context,
                                                 const void *result)
{
    (void)context;
    return PQnfields((const PGresult *)result);
}

static uint32_t vps_libpq_default_result_field_type(
    void *context, const void *result, int index)
{
    (void)context;
    return (uint32_t)PQftype((const PGresult *)result, index);
}

static const char *vps_libpq_default_result_field_name(
    void *context, const void *result, int index)
{
    (void)context;
    return PQfname((const PGresult *)result, index);
}

static int32_t vps_libpq_default_result_field_modifier(
    void *context, const void *result, int index)
{
    (void)context;
    return (int32_t)PQfmod((const PGresult *)result, index);
}

static uint32_t vps_libpq_default_result_field_relation(
    void *context, const void *result, int index)
{
    (void)context;
    return (uint32_t)PQftable((const PGresult *)result, index);
}

static int32_t vps_libpq_default_result_field_attribute(
    void *context, const void *result, int index)
{
    (void)context;
    return (int32_t)PQftablecol((const PGresult *)result, index);
}

static int vps_libpq_default_result_field_format(
    void *context, const void *result, int index)
{
    (void)context;
    return PQfformat((const PGresult *)result, index);
}

static int vps_libpq_default_result_row_count(void *context,
                                               const void *result)
{
    (void)context;
    return PQntuples((const PGresult *)result);
}

static const char *vps_libpq_default_result_command_tuples(
    void *context, const void *result)
{
    (void)context;
    return PQcmdTuples((PGresult *)result);
}

static int vps_libpq_default_result_value_is_null(
    void *context, const void *result, int row, int column)
{
    (void)context;
    return PQgetisnull((const PGresult *)result, row, column);
}

static const void *vps_libpq_default_result_value(
    void *context, const void *result, int row, int column)
{
    (void)context;
    return PQgetvalue((const PGresult *)result, row, column);
}

static int vps_libpq_default_result_value_length(
    void *context, const void *result, int row, int column)
{
    (void)context;
    return PQgetlength((const PGresult *)result, row, column);
}

static const char *vps_libpq_default_result_sqlstate(
    void *context, const void *result)
{
    (void)context;
    return PQresultErrorField((const PGresult *)result,
                              PG_DIAG_SQLSTATE);
}

static const char *vps_libpq_default_result_primary_message(
    void *context,
    const void *result)
{
    (void)context;
    return PQresultErrorField((const PGresult *)result,
                              PG_DIAG_MESSAGE_PRIMARY);
}

static void vps_libpq_default_clear_result(void *context, void *result)
{
    (void)context;
    PQclear((PGresult *)result);
}

static void vps_libpq_default_finish(void *context, void *connection)
{
    (void)context;
    PQfinish((PGconn *)connection);
}

const VpsLibpqClientApi *vps_libpq_client_default_api(void)
{
    static const VpsLibpqClientApi api = {
        sizeof(VpsLibpqClientApi),
        VPS_LIBPQ_CLIENT_API_VERSION,
        NULL,
        vps_libpq_default_connect_start,
        vps_libpq_default_set_nonblocking,
        vps_libpq_default_connect_poll,
        vps_libpq_default_socket_handle,
        vps_libpq_default_connection_status,
        vps_libpq_default_transaction_status,
        vps_libpq_default_pipeline_status,
        vps_libpq_default_error_class,
        vps_libpq_default_tls_verify,
        vps_libpq_default_identity_verify,
        vps_libpq_default_send_prepare,
        vps_libpq_default_send_describe_prepared,
        vps_libpq_default_send_query_prepared,
        vps_libpq_default_send_query_params,
        vps_libpq_default_set_single_row_mode,
        vps_libpq_default_cancel_create,
        vps_libpq_default_cancel_start,
        vps_libpq_default_cancel_poll,
        vps_libpq_default_cancel_socket,
        vps_libpq_default_cancel_reset,
        vps_libpq_default_cancel_finish,
        vps_libpq_default_flush,
        vps_libpq_default_consume_input,
        vps_libpq_default_is_busy,
        vps_libpq_default_get_result,
        vps_libpq_default_result_status,
        vps_libpq_default_result_parameter_count,
        vps_libpq_default_result_parameter_type,
        vps_libpq_default_result_field_count,
        vps_libpq_default_result_field_type,
        vps_libpq_default_result_field_name,
        vps_libpq_default_result_field_modifier,
        vps_libpq_default_result_field_relation,
        vps_libpq_default_result_field_attribute,
        vps_libpq_default_result_field_format,
        vps_libpq_default_result_row_count,
        vps_libpq_default_result_value_is_null,
        vps_libpq_default_result_value,
        vps_libpq_default_result_value_length,
        vps_libpq_default_result_sqlstate,
        vps_libpq_default_clear_result,
        vps_libpq_default_finish,
        vps_libpq_default_result_primary_message,
        vps_libpq_default_result_command_tuples};
    return &api;
}

int vps_libpq_client_library_version(void)
{
    return PQlibVersion();
}

static int vps_libpq_api_valid(const VpsLibpqClientApi *api)
{
    return api != NULL && api->structure_size >= sizeof(*api) &&
           api->api_version == VPS_LIBPQ_CLIENT_API_VERSION &&
           api->connect_start_params != NULL &&
           api->set_nonblocking != NULL && api->connect_poll != NULL &&
           api->socket_handle != NULL && api->connection_status != NULL &&
           api->transaction_status != NULL && api->pipeline_status != NULL &&
           api->connection_error_class != NULL && api->tls_verify != NULL &&
           api->identity_verify != NULL &&
           api->send_prepare != NULL &&
           api->send_describe_prepared != NULL &&
           api->send_query_prepared != NULL &&
           api->send_query_params != NULL && api->flush != NULL &&
           api->set_single_row_mode != NULL &&
           api->cancel_create != NULL && api->cancel_start != NULL &&
           api->cancel_poll != NULL && api->cancel_socket != NULL &&
           api->cancel_reset != NULL && api->cancel_finish != NULL &&
           api->consume_input != NULL && api->is_busy != NULL &&
           api->get_result != NULL && api->result_status != NULL &&
           api->result_parameter_count != NULL &&
           api->result_parameter_type != NULL &&
           api->result_field_count != NULL &&
           api->result_field_type != NULL &&
           api->result_field_name != NULL &&
           api->result_field_modifier != NULL &&
           api->result_field_relation != NULL &&
           api->result_field_attribute != NULL &&
           api->result_field_format != NULL &&
           api->result_row_count != NULL &&
           api->result_command_tuples != NULL &&
           api->result_value_is_null != NULL &&
           api->result_value != NULL &&
           api->result_value_length != NULL &&
           api->result_sqlstate != NULL && api->clear_result != NULL &&
           api->finish != NULL && api->result_primary_message != NULL;
}

static int vps_libpq_bounded_text(const char *value)
{
    size_t index;
    if (value == NULL) return 0;
    for (index = 0U; index <= VPS_CREDENTIAL_VALUE_MAX_LENGTH; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte == 0U) return index != 0U;
        if (byte < 0x20U || byte == 0x7fU) return 0;
    }
    return 0;
}

static int vps_libpq_config_valid(const VpsConnectionConfig *connection)
{
    const VpsCredentialConfig *config;
    size_t index;
    if (connection == NULL || !connection->initialized) return 0;
    config = &connection->config;
    if (config->header.structure_size < sizeof(*config) ||
        config->header.api_version != VPS_API_VERSION ||
        (config->header.present_fields & ~VPS_CREDENTIAL_FIELDS_CURRENT) !=
            0U) return 0;
    for (index = 0U; index < sizeof(vps_libpq_keyword_fields) /
                                  sizeof(vps_libpq_keyword_fields[0]);
         ++index) {
        const VpsLibpqKeywordField *field = &vps_libpq_keyword_fields[index];
        const char *const *member = (const char *const *)(
            (const unsigned char *)config + field->offset);
        if ((config->header.present_fields & field->field) != 0U &&
            !vps_libpq_bounded_text(*member)) return 0;
    }
    return 1;
}

VpsClientStatus vps_libpq_set_error(VpsError *error,
                                    VpsErrorClass error_class)
{
    if (error != NULL && error->initialized &&
        vps_error_set_local(error, VPS_ERROR_OPERATION_CONNECT, error_class,
                            NULL) != VPS_MEMORY_OK) {
        return VPS_CLIENT_OUT_OF_MEMORY;
    }
    return error_class == VPS_ERROR_CLASS_MEMORY
               ? VPS_CLIENT_OUT_OF_MEMORY
               : VPS_CLIENT_BACKEND_ERROR;
}

static uint64_t vps_libpq_elapsed_ms(const VpsLibpqConnection *connection)
{
    uint64_t now_ms;
    if (connection == NULL || !connection->deadline_started ||
        vps_platform_monotonic_now_ms(
            connection->client->platform_operations, &now_ms) !=
            VPS_PLATFORM_OK ||
        now_ms < connection->deadline.started_at_ms) return 0U;
    return now_ms - connection->deadline.started_at_ms;
}

static void vps_libpq_log(const VpsLibpqConnection *connection,
                          VpsLogLevel level,
                          const char *phase,
                          const char *status,
                          VpsErrorClass error_class)
{
    static const char operation[] = "libpq_connect";
    VpsLogEvent event;
    VpsLogger *logger;
    const char *fingerprint;
    if (connection == NULL || phase == NULL || status == NULL) return;
    logger = connection->client->logger;
    fingerprint = vps_identity_fingerprint(connection->client->identity);
    if (logger == NULL || fingerprint == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 strlen(phase)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_DURATION_MS,
                                 vps_libpq_elapsed_ms(connection)) !=
            VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_POLL_COUNT,
                                 connection->poll_count) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_WAIT_COUNT,
                                 connection->wait_count) != VPS_LOG_OK ||
        vps_log_event_add_string(&event,
                                 VPS_LOG_FIELD_CONNECTION_FINGERPRINT,
                                 fingerprint, strlen(fingerprint)) !=
            VPS_LOG_OK) return;
    if (error_class != VPS_ERROR_CLASS_NONE) {
        const char *error_name = vps_error_class_name(error_class);
        if (vps_log_event_add_string(&event, VPS_LOG_FIELD_ERROR_CLASS,
                                     error_name, strlen(error_name)) !=
            VPS_LOG_OK) return;
    }
    vps_logger_emit(logger, &event);
}

static VpsClientStatus vps_libpq_deadline_expired(
    VpsLibpqConnection *connection,
    int *expired,
    VpsError *error)
{
    uint64_t now_ms;
    uint64_t remaining_ms;
    if (vps_platform_monotonic_now_ms(
            connection->client->platform_operations, &now_ms) !=
            VPS_PLATFORM_OK ||
        vps_deadline_remaining_at(&connection->deadline, now_ms,
                                  &remaining_ms, expired) != VPS_DEADLINE_OK) {
        connection->phase = VPS_LIBPQ_PHASE_FAILED;
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_CONNECTION);
    }
    if (*expired) {
        connection->phase = VPS_LIBPQ_PHASE_FAILED;
        vps_libpq_log(connection, VPS_LOG_LEVEL_WARN, "deadline", "expired",
                      VPS_ERROR_CLASS_TIMEOUT);
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_TIMEOUT);
    }
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_libpq_keyword_view(
    const VpsLibpqClient *client,
    const char **keywords,
    const char **values)
{
    const VpsCredentialConfig *config = &client->connection_config->config;
    size_t count = 0U;
    size_t index;
    for (index = 0U; index < sizeof(vps_libpq_keyword_fields) /
                                  sizeof(vps_libpq_keyword_fields[0]);
         ++index) {
        const VpsLibpqKeywordField *field = &vps_libpq_keyword_fields[index];
        if ((config->header.present_fields & field->field) != 0U) {
            const char *const *member = (const char *const *)(
                (const unsigned char *)config + field->offset);
            if (count + 1U >= VPS_LIBPQ_CLIENT_MAX_KEYWORDS) {
                return VPS_CLIENT_LIMIT_EXCEEDED;
            }
            keywords[count] = field->keyword;
            values[count++] = *member;
        }
    }
    keywords[count] = "sslmode";
    values[count++] = vps_tls_mode_name(client->tls_policy.mode);
    keywords[count] = "channel_binding";
    values[count++] = vps_channel_binding_mode_name(
        client->tls_policy.channel_binding);
    keywords[count] = "client_encoding";
    values[count++] = "UTF8";
    keywords[count] = NULL;
    values[count] = NULL;
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_libpq_client_init(
    VpsLibpqClient *client,
    const VpsLibpqClientOptions *options)
{
    const VpsLibpqClientApi *api;
    if (client == NULL || options == NULL ||
        !vps_allocator_is_valid(options->allocator) ||
        vps_platform_validate_operations(
            options->platform_operations,
            VPS_PLATFORM_CAP_MONOTONIC_CLOCK |
                VPS_PLATFORM_CAP_SOCKET_WAIT) != VPS_PLATFORM_OK ||
        !vps_libpq_config_valid(options->connection_config) ||
        options->identity == NULL || !options->identity->initialized ||
        !options->identity->built || options->tls_policy == NULL ||
        !options->tls_policy->initialized || options->session_plan == NULL ||
        !options->session_plan->initialized || !options->session_plan->built ||
        options->connect_timeout_ms == 0U ||
        options->connect_timeout_ms >
            VPS_LIBPQ_CLIENT_MAX_CONNECT_TIMEOUT_MS ||
        options->cancel_timeout_ms >
            VPS_LIBPQ_CLIENT_MAX_CANCEL_TIMEOUT_MS ||
        options->wait_slice_ms == 0U ||
        options->wait_slice_ms > VPS_WAIT_MAX_SLICE_MS ||
        (options->reset_mode != VPS_LIBPQ_RESET_DISCARD_ALL &&
         options->reset_mode != VPS_LIBPQ_RESET_STRICT)) {
        return VPS_CLIENT_INVALID_ARGUMENT;
    }
    api = options->api == NULL ? vps_libpq_client_default_api()
                               : options->api;
    if (!vps_libpq_api_valid(api)) return VPS_CLIENT_INVALID_ARGUMENT;
    (void)memset(client, 0, sizeof(*client));
    client->allocator = *options->allocator;
    (void)memcpy(&client->api, api, sizeof(client->api));
    client->api.structure_size = sizeof(client->api);
    client->platform_operations = options->platform_operations;
    client->connection_config = options->connection_config;
    client->identity = options->identity;
    client->session_plan = options->session_plan;
    client->tls_policy = *options->tls_policy;
    client->interrupt_probe = options->interrupt_probe;
    client->interrupt_context = options->interrupt_context;
    client->logger = options->logger;
    client->connect_timeout_ms = options->connect_timeout_ms;
    client->cancel_timeout_ms =
        options->cancel_timeout_ms == 0U
            ? VPS_LIBPQ_CLIENT_DEFAULT_CANCEL_TIMEOUT_MS
            : options->cancel_timeout_ms;
    client->wait_slice_ms = options->wait_slice_ms;
    client->reset_mode = options->reset_mode;
    client->initialized = 1;
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_libpq_connection_create(void *context,
                                                    void **backend_connection,
                                                    VpsError *error)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqConnection *connection = NULL;
    (void)error;
    if (client == NULL || !client->initialized || backend_connection == NULL ||
        *backend_connection != NULL) return VPS_CLIENT_INVALID_ARGUMENT;
    if (vps_memory_allocate(&client->allocator, sizeof(*connection),
                            (void **)&connection) != VPS_MEMORY_OK) {
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_MEMORY);
    }
    (void)memset(connection, 0, sizeof(*connection));
    connection->client = client;
    connection->socket_handle = (intptr_t)-1;
    connection->phase = VPS_LIBPQ_PHASE_CREATED;
    client->connection_count += 1U;
    *backend_connection = connection;
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_libpq_connection_start(
    void *context,
    void *backend_connection,
    VpsClientOperation operation,
    VpsError *error)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqConnection *connection = (VpsLibpqConnection *)backend_connection;
    const char *keywords[VPS_LIBPQ_CLIENT_MAX_KEYWORDS];
    const char *values[VPS_LIBPQ_CLIENT_MAX_KEYWORDS];
    VpsClientStatus result;
    if (client == NULL || connection == NULL || connection->client != client) {
        return VPS_CLIENT_INVALID_STATE;
    }
    if ((operation == VPS_CLIENT_OPERATION_RESET ||
         operation == VPS_CLIENT_OPERATION_PING) &&
        connection->phase == VPS_LIBPQ_PHASE_READY) {
        if (vps_deadline_start(client->platform_operations,
                               client->connect_timeout_ms,
                               &connection->deadline) != VPS_DEADLINE_OK) {
            connection->phase = VPS_LIBPQ_PHASE_FAILED;
            return vps_libpq_set_error(error, VPS_ERROR_CLASS_CONNECTION);
        }
        connection->deadline_started = 1;
        return vps_libpq_health_begin(
            connection, operation,
            operation == VPS_CLIENT_OPERATION_RESET
                ? VPS_SESSION_PHASE_RESET
                : VPS_SESSION_PHASE_CONNECT,
            error);
    }
    if (operation != VPS_CLIENT_OPERATION_CONNECT ||
        connection->phase != VPS_LIBPQ_PHASE_CREATED) {
        return VPS_CLIENT_INVALID_STATE;
    }
    result = vps_libpq_keyword_view(client, keywords, values);
    if (result != VPS_CLIENT_OK) {
        connection->phase = VPS_LIBPQ_PHASE_FAILED;
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_CONFIG);
    }
    if (vps_deadline_start(client->platform_operations,
                           client->connect_timeout_ms,
                           &connection->deadline) != VPS_DEADLINE_OK) {
        connection->phase = VPS_LIBPQ_PHASE_FAILED;
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_CONNECTION);
    }
    connection->deadline_started = 1;
    connection->postgresql_connection = client->api.connect_start_params(
        client->api.context, keywords, values, 0);
    if (connection->postgresql_connection == NULL) {
        connection->phase = VPS_LIBPQ_PHASE_FAILED;
        vps_libpq_log(connection, VPS_LOG_LEVEL_ERROR, "start", "failed",
                      VPS_ERROR_CLASS_MEMORY);
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_MEMORY);
    }
    if (client->api.set_nonblocking(client->api.context,
                                    connection->postgresql_connection, 1) !=
        0) {
        connection->phase = VPS_LIBPQ_PHASE_FAILED;
        vps_libpq_log(connection, VPS_LOG_LEVEL_ERROR, "nonblocking",
                      "failed", VPS_ERROR_CLASS_CONNECTION);
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_CONNECTION);
    }
    connection->phase = VPS_LIBPQ_PHASE_CONNECTING;
    vps_libpq_log(connection, VPS_LOG_LEVEL_DEBUG, "start", "started",
                  VPS_ERROR_CLASS_NONE);
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_libpq_readiness(VpsLibpqConnection *connection,
                                            VpsError *error)
{
    VpsLibpqClient *client = connection->client;
    VpsTlsOutcome tls_outcome;
    VpsTlsResult tls_result;
    int expired = 0;
    VpsClientStatus result;
    if (client->api.connection_status(client->api.context,
                                      connection->postgresql_connection) !=
        VPS_LIBPQ_CONNECTION_OK) {
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_CONNECTION);
    }
    result = vps_libpq_deadline_expired(connection, &expired, error);
    if (result != VPS_CLIENT_OK) return result;
    (void)memset(&tls_outcome, 0, sizeof(tls_outcome));
    tls_result = client->api.tls_verify(
        client->api.context, connection->postgresql_connection,
        &client->tls_policy, &tls_outcome, client->logger);
    if (tls_result != VPS_TLS_OK) {
        connection->phase = VPS_LIBPQ_PHASE_FAILED;
        vps_libpq_log(connection, VPS_LOG_LEVEL_WARN, "tls", "failed",
                      VPS_ERROR_CLASS_TLS);
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_TLS);
    }
    if (!client->api.identity_verify(client->api.context,
                                     connection->postgresql_connection,
                                     client->identity)) {
        connection->phase = VPS_LIBPQ_PHASE_FAILED;
        vps_libpq_log(connection, VPS_LOG_LEVEL_ERROR, "identity", "failed",
                      VPS_ERROR_CLASS_CONFIG);
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_CONFIG);
    }
    result = vps_libpq_deadline_expired(connection, &expired, error);
    if (result != VPS_CLIENT_OK) return result;
    return vps_libpq_health_begin(connection, VPS_CLIENT_OPERATION_CONNECT,
                                  VPS_SESSION_PHASE_CONNECT, error);
}

static const char *vps_libpq_poll_name(VpsLibpqPollingStatus status)
{
    switch (status) {
    case VPS_LIBPQ_POLL_FAILED: return "failed";
    case VPS_LIBPQ_POLL_READING: return "reading";
    case VPS_LIBPQ_POLL_WRITING: return "writing";
    case VPS_LIBPQ_POLL_OK: return "ok";
    case VPS_LIBPQ_POLL_ACTIVE: return "active";
    default: return "invalid";
    }
}

static VpsClientStatus vps_libpq_connection_poll(
    void *context,
    void *backend_connection,
    VpsClientPollResult *poll_result,
    VpsError *error)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqConnection *connection = (VpsLibpqConnection *)backend_connection;
    size_t active_count = 0U;
    int expired = 0;
    VpsClientStatus result;
    if (client == NULL || connection == NULL || connection->client != client ||
        poll_result == NULL ||
        (connection->phase != VPS_LIBPQ_PHASE_CONNECTING &&
         connection->phase != VPS_LIBPQ_PHASE_HEALTH)) {
        return VPS_CLIENT_INVALID_STATE;
    }
    result = vps_libpq_deadline_expired(connection, &expired, error);
    if (result != VPS_CLIENT_OK) return result;
    if (connection->phase == VPS_LIBPQ_PHASE_HEALTH) {
        return vps_libpq_health_poll(connection, poll_result, error);
    }
    for (;;) {
        VpsLibpqPollingStatus status = client->api.connect_poll(
            client->api.context, connection->postgresql_connection);
        connection->poll_count += 1U;
        vps_libpq_log(connection,
                       status == VPS_LIBPQ_POLL_FAILED
                           ? VPS_LOG_LEVEL_ERROR
                           : VPS_LOG_LEVEL_DEBUG,
                       "poll", vps_libpq_poll_name(status),
                       status == VPS_LIBPQ_POLL_FAILED
                           ? VPS_ERROR_CLASS_CONNECTION
                           : VPS_ERROR_CLASS_NONE);
        if (status == VPS_LIBPQ_POLL_OK) {
            result = vps_libpq_readiness(connection, error);
            if (result != VPS_CLIENT_OK) return result;
            return vps_libpq_health_poll(connection, poll_result, error);
        }
        if (status == VPS_LIBPQ_POLL_FAILED ||
            (status != VPS_LIBPQ_POLL_READING &&
             status != VPS_LIBPQ_POLL_WRITING &&
             status != VPS_LIBPQ_POLL_ACTIVE)) {
            VpsErrorClass error_class = client->api.connection_error_class(
                client->api.context, connection->postgresql_connection);
            if (error_class < VPS_ERROR_CLASS_CONFIG ||
                error_class > VPS_ERROR_CLASS_UNSUPPORTED) {
                error_class = VPS_ERROR_CLASS_CONNECTION;
            }
            connection->phase = VPS_LIBPQ_PHASE_FAILED;
            return vps_libpq_set_error(error, error_class);
        }
        if (status == VPS_LIBPQ_POLL_ACTIVE &&
            active_count++ < VPS_LIBPQ_CLIENT_MAX_ACTIVE_POLLS) {
            result = vps_libpq_deadline_expired(connection, &expired, error);
            if (result != VPS_CLIENT_OK) return result;
            continue;
        }
        connection->socket_handle = client->api.socket_handle(
            client->api.context, connection->postgresql_connection);
        if (connection->socket_handle < 0) {
            connection->phase = VPS_LIBPQ_PHASE_FAILED;
            return vps_libpq_set_error(error, VPS_ERROR_CLASS_CONNECTION);
        }
        connection->wait_interest =
            status == VPS_LIBPQ_POLL_READING
                ? VPS_CLIENT_WAIT_READ
                : status == VPS_LIBPQ_POLL_WRITING
                      ? VPS_CLIENT_WAIT_WRITE
                      : VPS_CLIENT_WAIT_READ_WRITE;
        (void)memset(poll_result, 0, sizeof(*poll_result));
        poll_result->outcome = VPS_CLIENT_POLL_WAIT;
        poll_result->wait.interest = connection->wait_interest;
        poll_result->wait.phase = VPS_CLIENT_WAIT_CONNECT;
        poll_result->wait.max_slice_ms = client->wait_slice_ms;
        return VPS_CLIENT_OK;
    }
}

static VpsClientStatus vps_libpq_connection_wait(
    void *context,
    void *backend_connection,
    const VpsClientWaitRequest *wait_request,
    VpsError *error)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqConnection *connection = (VpsLibpqConnection *)backend_connection;
    VpsSocketWaitRequest request;
    VpsSocketWaitResult wait_result;
    VpsDeadlineStatus deadline_status;
    if (client == NULL || connection == NULL || connection->client != client ||
        wait_request == NULL ||
        (connection->phase != VPS_LIBPQ_PHASE_CONNECTING &&
         connection->phase != VPS_LIBPQ_PHASE_HEALTH) ||
        wait_request->phase !=
            (connection->phase == VPS_LIBPQ_PHASE_CONNECTING
                 ? VPS_CLIENT_WAIT_CONNECT
                 : connection->active_operation == VPS_CLIENT_OPERATION_RESET
                       ? VPS_CLIENT_WAIT_RESET
                       : connection->active_operation ==
                                 VPS_CLIENT_OPERATION_PING
                             ? VPS_CLIENT_WAIT_PING
                             : VPS_CLIENT_WAIT_CONNECT) ||
        wait_request->interest != connection->wait_interest) {
        return VPS_CLIENT_INVALID_STATE;
    }
    (void)memset(&request, 0, sizeof(request));
    request.operations = client->platform_operations;
    request.socket_handle = connection->socket_handle;
    request.interest = (VpsWaitInterest)wait_request->interest;
    request.deadline = &connection->deadline;
    request.max_slice_ms = wait_request->max_slice_ms;
    request.interrupt_probe = client->interrupt_probe;
    request.interrupt_context = client->interrupt_context;
    request.logger = client->logger;
    request.phase = connection->active_operation == VPS_CLIENT_OPERATION_RESET
                        ? VPS_WAIT_PHASE_RESET
                        : connection->active_operation ==
                                  VPS_CLIENT_OPERATION_PING
                              ? VPS_WAIT_PHASE_PING
                              : VPS_WAIT_PHASE_CONNECT;
    deadline_status = vps_socket_wait_execute(&request, &wait_result);
    connection->wait_count += wait_result.wait_count;
    if (deadline_status == VPS_DEADLINE_OK &&
        wait_result.outcome == VPS_SOCKET_WAIT_READY) {
        vps_libpq_log(connection, VPS_LOG_LEVEL_DEBUG, "wait", "ready",
                      VPS_ERROR_CLASS_NONE);
        return VPS_CLIENT_OK;
    }
    connection->phase = VPS_LIBPQ_PHASE_FAILED;
    if (deadline_status == VPS_DEADLINE_OK &&
        wait_result.outcome == VPS_SOCKET_WAIT_DEADLINE_EXPIRED) {
        vps_libpq_log(connection, VPS_LOG_LEVEL_WARN, "wait", "timeout",
                      VPS_ERROR_CLASS_TIMEOUT);
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_TIMEOUT);
    }
    if (deadline_status == VPS_DEADLINE_OK &&
        wait_result.outcome == VPS_SOCKET_WAIT_INTERRUPTED) {
        vps_libpq_log(connection, VPS_LOG_LEVEL_INFO, "wait", "interrupted",
                      VPS_ERROR_CLASS_CANCEL);
        return vps_libpq_set_error(error, VPS_ERROR_CLASS_CANCEL);
    }
    vps_libpq_log(connection, VPS_LOG_LEVEL_ERROR, "wait", "failed",
                  VPS_ERROR_CLASS_CONNECTION);
    return vps_libpq_set_error(error, VPS_ERROR_CLASS_CONNECTION);
}

static void vps_libpq_connection_destroy(void *context,
                                          void *backend_connection)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqConnection *connection = (VpsLibpqConnection *)backend_connection;
    if (client == NULL || connection == NULL || connection->client != client) {
        return;
    }
    if (connection->postgresql_connection != NULL) {
        client->api.finish(client->api.context,
                           connection->postgresql_connection);
        connection->postgresql_connection = NULL;
    }
    if (client->connection_count != 0U) client->connection_count -= 1U;
    vps_memory_release(&client->allocator, (void **)&connection,
                       sizeof(*connection));
}

VpsClientStatus vps_libpq_client_make_operations(
    const VpsLibpqClient *client,
    VpsClientOperations *operations)
{
    if (client == NULL || !client->initialized || operations == NULL) {
        return VPS_CLIENT_INVALID_ARGUMENT;
    }
    (void)memset(operations, 0, sizeof(*operations));
    operations->structure_size = sizeof(*operations);
    operations->contract_version = VPS_CLIENT_CONTRACT_VERSION;
    operations->capabilities = VPS_CLIENT_CAP_CONNECT |
                               VPS_CLIENT_CAP_PREPARE |
                               VPS_CLIENT_CAP_EXECUTE |
                               VPS_CLIENT_CAP_FETCH |
                               VPS_CLIENT_CAP_RESET |
                               VPS_CLIENT_CAP_PING |
                               VPS_CLIENT_CAP_CANCEL;
    operations->connection_create = vps_libpq_connection_create;
    operations->connection_start = vps_libpq_connection_start;
    operations->connection_poll = vps_libpq_connection_poll;
    operations->connection_wait = vps_libpq_connection_wait;
    operations->connection_destroy = vps_libpq_connection_destroy;
    operations->statement_create = vps_libpq_statement_create;
    operations->statement_start = vps_libpq_statement_start;
    operations->statement_poll = vps_libpq_statement_poll;
    operations->statement_wait = vps_libpq_statement_wait;
    operations->statement_metadata = vps_libpq_statement_metadata;
    operations->statement_result_field = vps_libpq_statement_result_field;
    operations->statement_row = vps_libpq_statement_row;
    operations->statement_column = vps_libpq_statement_column;
    operations->statement_row_release = vps_libpq_statement_row_release;
    operations->statement_destroy = vps_libpq_statement_destroy;
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_libpq_client_cleanup(VpsLibpqClient *client)
{
    if (client == NULL) return VPS_CLIENT_INVALID_ARGUMENT;
    if (!client->initialized) return VPS_CLIENT_OK;
    if (client->connection_count != 0U) return VPS_CLIENT_INVALID_STATE;
    (void)memset(client, 0, sizeof(*client));
    return VPS_CLIENT_OK;
}
