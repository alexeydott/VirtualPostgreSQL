#include "vps_libpq_client_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define VPS_LIBPQ_STATEMENT_NAME_SIZE 48U
#define VPS_LIBPQ_STATEMENT_MAX_INTERNAL_POLLS 8U
#define VPS_LIBPQ_PRIMARY_MESSAGE_INPUT_LIMIT 4096U

typedef enum VpsLibpqStatementPhase {
    VPS_LIBPQ_STATEMENT_CREATED = 0,
    VPS_LIBPQ_STATEMENT_PREPARE_FLUSH = 1,
    VPS_LIBPQ_STATEMENT_PREPARE_WAIT = 2,
    VPS_LIBPQ_STATEMENT_PREPARE_RESULT = 3,
    VPS_LIBPQ_STATEMENT_DESCRIBE_FLUSH = 4,
    VPS_LIBPQ_STATEMENT_DESCRIBE_WAIT = 5,
    VPS_LIBPQ_STATEMENT_DESCRIBE_RESULT = 6,
    VPS_LIBPQ_STATEMENT_PREPARED = 7,
    VPS_LIBPQ_STATEMENT_EXECUTE_FLUSH = 8,
    VPS_LIBPQ_STATEMENT_EXECUTE_WAIT = 9,
    VPS_LIBPQ_STATEMENT_EXECUTE_RESULT = 10,
    VPS_LIBPQ_STATEMENT_FETCH_WAIT = 11,
    VPS_LIBPQ_STATEMENT_ROW_READY = 12,
    VPS_LIBPQ_STATEMENT_COMPLETE = 13,
    VPS_LIBPQ_STATEMENT_CANCEL_POLL = 14,
    VPS_LIBPQ_STATEMENT_CANCEL_DRAIN = 15,
    VPS_LIBPQ_STATEMENT_FAILED = 16
} VpsLibpqStatementPhase;

typedef struct VpsLibpqDescribedField {
    VpsClientResultFieldMetadata metadata;
    char name[VPS_CLIENT_MAX_FIELD_NAME_BYTES + 1U];
} VpsLibpqDescribedField;

typedef struct VpsLibpqStatement {
    VpsLibpqConnection *connection;
    VpsClientStatementSpec spec;
    uint32_t *parameter_types;
    const char **parameter_values;
    int *parameter_lengths;
    int *parameter_formats;
    size_t parameter_types_size;
    size_t parameter_values_size;
    size_t parameter_lengths_size;
    size_t parameter_formats_size;
    VpsLibpqDescribedField *described_fields;
    size_t described_fields_size;
    size_t described_field_count;
    VpsDeadline deadline;
    void *current_result;
    void *cancel_connection;
    intptr_t socket_handle;
    VpsClientWaitInterest wait_interest;
    VpsClientWaitPhase wait_phase;
    VpsLibpqStatementPhase phase;
    uint64_t poll_count;
    uint64_t wait_count;
    uint64_t query_hash;
    uint64_t published_row_count;
    uint64_t published_byte_count;
    uint64_t affected_count;
    size_t current_row_bytes;
    char query_fingerprint[VPS_ERROR_FINGERPRINT_BUFFER_SIZE];
    char prepared_name[VPS_LIBPQ_STATEMENT_NAME_SIZE];
    int native_parameter_count;
    int described;
    int affected_count_valid;
    int deadline_started;
    int cancel_sqlstate_seen;
    int cancel_terminal_seen;
} VpsLibpqStatement;

static VpsErrorOperation vps_libpq_statement_error_operation(
    const VpsLibpqStatement *statement)
{
    return statement->spec.error_operation == VPS_ERROR_OPERATION_NONE
               ? VPS_ERROR_OPERATION_QUERY
               : statement->spec.error_operation;
}

static void vps_libpq_statement_log(const VpsLibpqStatement *statement,
                                    VpsLogLevel level,
                                    const char *operation,
                                    const char *phase,
                                    const char *status,
                                    const char *sqlstate);
static void vps_libpq_statement_log_server_error(
    const VpsLibpqStatement *statement,
    const void *result,
    const char *sqlstate);

static uint64_t vps_libpq_hash_lane(const char *value,
                                    size_t length,
                                    uint64_t seed)
{
    static const uint64_t prime = UINT64_C(1099511628211);
    uint64_t hash = seed;
    size_t index;
    for (index = 0U; index < length; ++index) {
        hash ^= (uint64_t)(unsigned char)value[index];
        hash *= prime;
    }
    return hash;
}

static int vps_libpq_statement_fingerprint(VpsLibpqStatement *statement)
{
    static const uint64_t seeds[4] = {
        UINT64_C(14695981039346656037), UINT64_C(1099511628211),
        UINT64_C(7809847782465536322), UINT64_C(9650029242287828579)};
    size_t lane;
    for (lane = 0U; lane < 4U; ++lane) {
        uint64_t hash = vps_libpq_hash_lane(
            statement->spec.query, statement->spec.query_length,
            seeds[lane] ^ (uint64_t)lane);
        if (snprintf(statement->query_fingerprint + (lane * 16U), 17U,
                     "%016" PRIx64, hash) != 16) return 0;
        if (lane == 0U) statement->query_hash = hash;
    }
    statement->query_fingerprint[VPS_ERROR_FINGERPRINT_LENGTH] = '\0';
    return 1;
}

static VpsClientStatus vps_libpq_statement_error(
    VpsLibpqStatement *statement,
    VpsError *error,
    VpsErrorClass error_class,
    const char *sqlstate)
{
    VpsMemoryResult error_result = VPS_MEMORY_OK;
    if (statement->cancel_connection != NULL) {
        statement->connection->client->api.cancel_finish(
            statement->connection->client->api.context,
            statement->cancel_connection);
        statement->cancel_connection = NULL;
    }
    vps_libpq_statement_log(
        statement, VPS_LOG_LEVEL_WARN, "libpq_statement",
        statement->phase == VPS_LIBPQ_STATEMENT_CANCEL_POLL ||
                statement->phase == VPS_LIBPQ_STATEMENT_CANCEL_DRAIN
            ? "cancel"
            : "error",
        vps_error_class_name(error_class), sqlstate);
    statement->phase = VPS_LIBPQ_STATEMENT_FAILED;
    statement->connection->phase = VPS_LIBPQ_PHASE_FAILED;
    if (error != NULL && error->initialized) {
        if (sqlstate != NULL) {
            error_result = vps_error_set_sqlstate(
                error, vps_libpq_statement_error_operation(statement),
                sqlstate, 0, 0,
                statement->query_fingerprint);
        } else {
            error_result = vps_error_set_local(
                error, vps_libpq_statement_error_operation(statement),
                error_class,
                statement->query_fingerprint);
        }
    }
    if (error_result != VPS_MEMORY_OK) return VPS_CLIENT_OUT_OF_MEMORY;
    return error_class == VPS_ERROR_CLASS_MEMORY
               ? VPS_CLIENT_OUT_OF_MEMORY
               : error_class == VPS_ERROR_CLASS_CONFIG
                     ? VPS_CLIENT_LIMIT_EXCEEDED
                     : VPS_CLIENT_BACKEND_ERROR;
}

static VpsClientStatus vps_libpq_statement_control_signal(
    VpsLibpqStatement *statement,
    VpsError *error,
    VpsErrorClass error_class)
{
    if (statement == NULL ||
        (error_class != VPS_ERROR_CLASS_TIMEOUT &&
         error_class != VPS_ERROR_CLASS_CANCEL))
        return VPS_CLIENT_INVALID_ARGUMENT;
    vps_libpq_statement_log(statement, VPS_LOG_LEVEL_INFO,
                            "libpq_statement", "control",
                            vps_error_class_name(error_class), NULL);
    if (error != NULL && error->initialized &&
        vps_error_set_local(error,
                            vps_libpq_statement_error_operation(statement),
                            error_class,
                            statement->query_fingerprint) != VPS_MEMORY_OK)
        return VPS_CLIENT_OUT_OF_MEMORY;
    return VPS_CLIENT_CONTROL_SIGNAL;
}

static uint64_t vps_libpq_statement_elapsed_ms(
    const VpsLibpqStatement *statement)
{
    uint64_t now_ms;
    if (!statement->deadline_started ||
        vps_platform_monotonic_now_ms(
            statement->connection->client->platform_operations, &now_ms) !=
            VPS_PLATFORM_OK ||
        now_ms < statement->deadline.started_at_ms) return 0U;
    return now_ms - statement->deadline.started_at_ms;
}

static void vps_libpq_statement_log(const VpsLibpqStatement *statement,
                                    VpsLogLevel level,
                                    const char *operation,
                                    const char *phase,
                                    const char *status,
                                    const char *sqlstate)
{
    VpsLogEvent event;
    VpsLogger *logger = statement->connection->client->logger;
    if (logger == NULL || operation == NULL || phase == NULL ||
        status == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 strlen(operation)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 strlen(phase)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_DURATION_MS,
                                 vps_libpq_statement_elapsed_ms(statement)) !=
            VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_POLL_COUNT,
                                 statement->poll_count) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_WAIT_COUNT,
                                 statement->wait_count) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PARAMETER_COUNT,
                                 statement->spec.parameter_count) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_RESULT_FIELD_COUNT,
                                 statement->spec.result_field_count) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_ROW_COUNT,
                                 statement->published_row_count) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_BYTE_COUNT,
                                 statement->published_byte_count) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_QUERY_FINGERPRINT,
                                 statement->query_fingerprint,
                                 VPS_ERROR_FINGERPRINT_LENGTH) != VPS_LOG_OK) {
        return;
    }
    if (sqlstate != NULL &&
        vps_log_event_add_string(&event, VPS_LOG_FIELD_SQLSTATE, sqlstate,
                                 VPS_SQLSTATE_LENGTH) != VPS_LOG_OK) return;
    if (level == VPS_LOG_LEVEL_DEBUG) {
        VpsLogResult sql_result = vps_log_event_add_debug_sql(
            &event, statement->spec.query, statement->spec.query_length);
        if (sql_result != VPS_LOG_OK && sql_result != VPS_LOG_REDACTED &&
            sql_result != VPS_LOG_LIMIT_EXCEEDED) {
            return;
        }
    }
    vps_logger_emit(logger, &event);
}

static void vps_libpq_statement_log_server_error(
    const VpsLibpqStatement *statement,
    const void *result,
    const char *sqlstate)
{
    static const char operation[] = "libpq_server_error";
    static const char phase[] = "result";
    static const char status[] = "failed";
    VpsLibpqClient *client = statement->connection->client;
    VpsLogger *logger = client->logger;
    const char *primary;
    size_t primary_length;
    char redacted_primary[VPS_LOG_MAX_STRING_LENGTH + 1U];
    VpsLogEvent event;
    VpsLogResult diagnostic_result;
    if (logger == NULL || result == NULL) return;
    primary = client->api.result_primary_message(client->api.context,
                                                  result);
    if (primary == NULL) return;
    for (primary_length = 0U;
         primary_length <= VPS_LIBPQ_PRIMARY_MESSAGE_INPUT_LIMIT &&
         primary[primary_length] != '\0';
         ++primary_length) {
    }
    if (primary_length == 0U ||
        primary_length > VPS_LIBPQ_PRIMARY_MESSAGE_INPUT_LIMIT ||
        vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 sizeof(phase) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 sizeof(status) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_QUERY_FINGERPRINT,
                                 statement->query_fingerprint,
                                 VPS_ERROR_FINGERPRINT_LENGTH) != VPS_LOG_OK) {
        return;
    }
    diagnostic_result = vps_log_event_add_primary_message(
        &event, primary, primary_length, redacted_primary,
        sizeof(redacted_primary));
    if (diagnostic_result != VPS_LOG_OK &&
        diagnostic_result != VPS_LOG_REDACTED) return;
    if (sqlstate != NULL &&
        vps_log_event_add_string(&event, VPS_LOG_FIELD_SQLSTATE, sqlstate,
                                 VPS_SQLSTATE_LENGTH) != VPS_LOG_OK) {
        return;
    }
    diagnostic_result = vps_log_event_add_debug_sql(
        &event, statement->spec.query, statement->spec.query_length);
    if (diagnostic_result != VPS_LOG_OK &&
        diagnostic_result != VPS_LOG_REDACTED &&
        diagnostic_result != VPS_LOG_LIMIT_EXCEEDED) {
        return;
    }
    vps_logger_emit(logger, &event);
}

static VpsClientStatus vps_libpq_statement_deadline(
    VpsLibpqStatement *statement,
    VpsError *error)
{
    uint64_t now_ms;
    uint64_t remaining_ms;
    int expired;
    if (!statement->deadline_started ||
        vps_platform_monotonic_now_ms(
            statement->connection->client->platform_operations, &now_ms) !=
            VPS_PLATFORM_OK ||
        vps_deadline_remaining_at(&statement->deadline, now_ms,
                                  &remaining_ms, &expired) != VPS_DEADLINE_OK) {
        return vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
    }
    if (!expired) return VPS_CLIENT_OK;
    vps_libpq_statement_log(statement, VPS_LOG_LEVEL_WARN, "libpq_statement",
                            "deadline", "expired", NULL);
    return vps_libpq_statement_error(statement, error,
                                     VPS_ERROR_CLASS_TIMEOUT, NULL);
}

static VpsClientStatus vps_libpq_statement_allocate_array(
    VpsLibpqStatement *statement,
    size_t element_size,
    void **array,
    size_t *allocated_size,
    VpsError *error)
{
    if (vps_size_multiply(statement->spec.parameter_count, element_size,
                          allocated_size) != VPS_MEMORY_OK ||
        vps_memory_allocate(&statement->connection->client->allocator,
                            *allocated_size, array) != VPS_MEMORY_OK) {
        return vps_libpq_statement_error(statement, error,
                                         VPS_ERROR_CLASS_MEMORY, NULL);
    }
    return VPS_CLIENT_OK;
}

static void vps_libpq_statement_release_arrays(
    VpsLibpqStatement *statement)
{
    VpsAllocator *allocator = &statement->connection->client->allocator;
    vps_memory_release(allocator, (void **)&statement->described_fields,
                       statement->described_fields_size);
    statement->described_field_count = 0U;
    vps_memory_release(allocator, (void **)&statement->parameter_formats,
                       statement->parameter_formats_size);
    vps_memory_release(allocator, (void **)&statement->parameter_lengths,
                       statement->parameter_lengths_size);
    vps_memory_release(allocator, (void **)&statement->parameter_values,
                       statement->parameter_values_size);
    vps_memory_release(allocator, (void **)&statement->parameter_types,
                       statement->parameter_types_size);
}

static VpsClientStatus vps_libpq_statement_build_parameters(
    VpsLibpqStatement *statement,
    VpsError *error)
{
    size_t index;
    VpsClientStatus result;
    if (vps_size_to_int(statement->spec.parameter_count,
                        &statement->native_parameter_count) != VPS_MEMORY_OK) {
        return vps_libpq_statement_error(statement, error,
                                         VPS_ERROR_CLASS_CONFIG, NULL);
    }
    if (statement->spec.parameter_count == 0U) return VPS_CLIENT_OK;
    result = vps_libpq_statement_allocate_array(
        statement, sizeof(*statement->parameter_types),
        (void **)&statement->parameter_types,
        &statement->parameter_types_size, error);
    if (result != VPS_CLIENT_OK) return result;
    result = vps_libpq_statement_allocate_array(
        statement, sizeof(*statement->parameter_values),
        (void **)&statement->parameter_values,
        &statement->parameter_values_size, error);
    if (result != VPS_CLIENT_OK) return result;
    result = vps_libpq_statement_allocate_array(
        statement, sizeof(*statement->parameter_lengths),
        (void **)&statement->parameter_lengths,
        &statement->parameter_lengths_size, error);
    if (result != VPS_CLIENT_OK) return result;
    result = vps_libpq_statement_allocate_array(
        statement, sizeof(*statement->parameter_formats),
        (void **)&statement->parameter_formats,
        &statement->parameter_formats_size, error);
    if (result != VPS_CLIENT_OK) return result;
    for (index = 0U; index < statement->spec.parameter_count; ++index) {
        const VpsClientParameterView *parameter =
            &statement->spec.parameters[index];
        int length;
        if (vps_size_to_int(parameter->length, &length) != VPS_MEMORY_OK) {
            return vps_libpq_statement_error(
                statement, error, VPS_ERROR_CLASS_CONFIG, NULL);
        }
        statement->parameter_types[index] = parameter->type_oid;
        statement->parameter_values[index] = parameter->is_null
                                                 ? NULL
                                                 : (const char *)parameter->value;
        statement->parameter_lengths[index] = length;
        statement->parameter_formats[index] = (int)parameter->format;
    }
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_libpq_statement_create(
    void *context,
    void *connection_value,
    const VpsClientStatementSpec *spec,
    void **statement_value,
    VpsError *error)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqConnection *connection =
        (VpsLibpqConnection *)connection_value;
    VpsLibpqStatement *statement = NULL;
    VpsClientStatus result;
    if (client == NULL || !client->initialized || connection == NULL ||
        connection->client != client || connection->phase != VPS_LIBPQ_PHASE_READY ||
        connection->statement_count != 0U || spec == NULL ||
        statement_value == NULL || *statement_value != NULL) {
        return VPS_CLIENT_INVALID_STATE;
    }
    if (vps_memory_allocate(&client->allocator, sizeof(*statement),
                            (void **)&statement) != VPS_MEMORY_OK) {
        return VPS_CLIENT_OUT_OF_MEMORY;
    }
    (void)memset(statement, 0, sizeof(*statement));
    statement->connection = connection;
    statement->spec = *spec;
    statement->socket_handle = (intptr_t)-1;
    statement->phase = VPS_LIBPQ_STATEMENT_CREATED;
    if (!vps_libpq_statement_fingerprint(statement)) {
        result = vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_INVARIANT, NULL);
        goto fail;
    }
    if (client->statement_serial == UINT64_MAX) {
        result = vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_CONFIG, NULL);
        goto fail;
    }
    client->statement_serial += 1U;
    if (snprintf(statement->prepared_name,
                 sizeof(statement->prepared_name), "vps_%016" PRIx64
                 "_%.16s", client->statement_serial,
                 statement->query_fingerprint) <= 0) {
        result = vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_INVARIANT, NULL);
        goto fail;
    }
    result = vps_libpq_statement_build_parameters(statement, error);
    if (result != VPS_CLIENT_OK) goto fail;
    connection->statement_count += 1U;
    *statement_value = statement;
    vps_libpq_statement_log(statement, VPS_LOG_LEVEL_DEBUG,
                            "libpq_statement", "create", "ok", NULL);
    return VPS_CLIENT_OK;

fail:
    vps_libpq_statement_release_arrays(statement);
    vps_memory_release(&client->allocator, (void **)&statement,
                       sizeof(*statement));
    return result;
}

static int vps_libpq_statement_result_format(
    const VpsLibpqStatement *statement)
{
    return statement->spec.result_field_count == 0U
               ? (int)VPS_CLIENT_VALUE_TEXT
               : (int)statement->spec.result_fields[0].format;
}

VpsClientStatus vps_libpq_statement_start(
    void *context,
    void *statement_value,
    VpsClientOperation operation,
    VpsError *error)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqStatement *statement = (VpsLibpqStatement *)statement_value;
    int send_result;
    if (client == NULL || statement == NULL ||
        statement->connection->client != client ||
        statement->connection->phase != VPS_LIBPQ_PHASE_READY) {
        return VPS_CLIENT_INVALID_STATE;
    }
    if (operation == VPS_CLIENT_OPERATION_PREPARE) {
        if (!statement->spec.prepare ||
            statement->phase != VPS_LIBPQ_STATEMENT_CREATED) {
            return VPS_CLIENT_INVALID_STATE;
        }
    } else if (operation == VPS_CLIENT_OPERATION_EXECUTE) {
        if ((statement->spec.prepare &&
             statement->phase != VPS_LIBPQ_STATEMENT_PREPARED) ||
            (!statement->spec.prepare &&
             statement->phase != VPS_LIBPQ_STATEMENT_CREATED)) {
            return VPS_CLIENT_INVALID_STATE;
        }
    } else if (operation == VPS_CLIENT_OPERATION_FETCH) {
        if (!statement->spec.single_row ||
            statement->phase != VPS_LIBPQ_STATEMENT_FETCH_WAIT) {
            return VPS_CLIENT_INVALID_STATE;
        }
        return VPS_CLIENT_OK;
    } else if (operation == VPS_CLIENT_OPERATION_CANCEL) {
        if (statement->phase == VPS_LIBPQ_STATEMENT_CREATED ||
            statement->phase == VPS_LIBPQ_STATEMENT_PREPARED ||
            statement->phase == VPS_LIBPQ_STATEMENT_COMPLETE ||
            statement->phase == VPS_LIBPQ_STATEMENT_FAILED ||
            statement->phase == VPS_LIBPQ_STATEMENT_CANCEL_POLL ||
            statement->phase == VPS_LIBPQ_STATEMENT_CANCEL_DRAIN) {
            return VPS_CLIENT_INVALID_STATE;
        }
        if (statement->current_result != NULL) {
            client->api.clear_result(client->api.context,
                                     statement->current_result);
            statement->current_result = NULL;
            statement->current_row_bytes = 0U;
        }
        if (vps_deadline_start(client->platform_operations,
                               client->cancel_timeout_ms,
                               &statement->deadline) != VPS_DEADLINE_OK) {
            return vps_libpq_statement_error(
                statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
        }
        statement->deadline_started = 1;
        statement->cancel_connection = client->api.cancel_create(
            client->api.context,
            statement->connection->postgresql_connection);
        if (statement->cancel_connection == NULL ||
            client->api.cancel_start(client->api.context,
                                     statement->cancel_connection) != 1) {
            return vps_libpq_statement_error(
                statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
        }
        statement->cancel_sqlstate_seen = 0;
        statement->cancel_terminal_seen = 0;
        statement->phase = VPS_LIBPQ_STATEMENT_CANCEL_POLL;
        vps_libpq_statement_log(statement, VPS_LOG_LEVEL_INFO,
                                "libpq_statement", "cancel", "started",
                                NULL);
        return VPS_CLIENT_OK;
    } else {
        return VPS_CLIENT_UNSUPPORTED;
    }
    if (vps_deadline_start(client->platform_operations,
                           statement->spec.timeout_ms,
                           &statement->deadline) != VPS_DEADLINE_OK) {
        return vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
    }
    statement->deadline_started = 1;
    statement->poll_count = 0U;
    statement->wait_count = 0U;
    if (operation == VPS_CLIENT_OPERATION_FETCH) {
        return VPS_CLIENT_OK;
    }
    if (operation == VPS_CLIENT_OPERATION_PREPARE) {
        send_result = client->api.send_prepare(
            client->api.context, statement->connection->postgresql_connection,
            statement->prepared_name, statement->spec.query,
            statement->native_parameter_count, statement->parameter_types);
        statement->phase = VPS_LIBPQ_STATEMENT_PREPARE_FLUSH;
    } else if (statement->spec.prepare) {
        send_result = client->api.send_query_prepared(
            client->api.context, statement->connection->postgresql_connection,
            statement->prepared_name, statement->native_parameter_count,
            statement->parameter_values, statement->parameter_lengths,
            statement->parameter_formats,
            vps_libpq_statement_result_format(statement));
        statement->phase = VPS_LIBPQ_STATEMENT_EXECUTE_FLUSH;
    } else {
        send_result = client->api.send_query_params(
            client->api.context, statement->connection->postgresql_connection,
            statement->spec.query, statement->native_parameter_count,
            statement->parameter_types, statement->parameter_values,
            statement->parameter_lengths, statement->parameter_formats,
            vps_libpq_statement_result_format(statement));
        statement->phase = VPS_LIBPQ_STATEMENT_EXECUTE_FLUSH;
    }
    if (send_result != 1) {
        return vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
    }
    if (operation == VPS_CLIENT_OPERATION_EXECUTE &&
        statement->spec.single_row &&
        client->api.set_single_row_mode(
            client->api.context,
            statement->connection->postgresql_connection) != 1) {
        return vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
    }
    vps_libpq_statement_log(
        statement, VPS_LOG_LEVEL_DEBUG, "libpq_statement",
        operation == VPS_CLIENT_OPERATION_PREPARE ? "prepare" : "execute",
        "started", NULL);
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_libpq_statement_wait_result(
    VpsLibpqStatement *statement,
    VpsClientPollResult *result,
    VpsClientWaitInterest interest,
    VpsClientWaitPhase phase)
{
    VpsLibpqClient *client = statement->connection->client;
    statement->socket_handle =
        statement->phase == VPS_LIBPQ_STATEMENT_CANCEL_POLL
            ? client->api.cancel_socket(client->api.context,
                                        statement->cancel_connection)
            : client->api.socket_handle(
                  client->api.context,
                  statement->connection->postgresql_connection);
    if (statement->socket_handle < 0) return VPS_CLIENT_BACKEND_ERROR;
    statement->wait_interest = interest;
    statement->wait_phase = phase;
    (void)memset(result, 0, sizeof(*result));
    result->outcome = VPS_CLIENT_POLL_WAIT;
    result->wait.interest = interest;
    result->wait.phase = phase;
    result->wait.max_slice_ms = client->wait_slice_ms;
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_libpq_statement_check_metadata(
    VpsLibpqStatement *statement,
    const void *result,
    int check_parameters,
    VpsError *error)
{
    VpsLibpqClient *client = statement->connection->client;
    int field_count = client->api.result_field_count(client->api.context,
                                                      result);
    size_t index;
    if (field_count < 0 || (size_t)field_count >
                               VPS_CLIENT_MAX_RESULT_FIELD_COUNT ||
        (!statement->spec.discover_result_fields &&
         (size_t)field_count != statement->spec.result_field_count)) {
        return vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_SCHEMA, NULL);
    }
    if (check_parameters) {
        int parameter_count = client->api.result_parameter_count(
            client->api.context, result);
        if (parameter_count < 0 || (size_t)parameter_count !=
                                       statement->spec.parameter_count) {
            return vps_libpq_statement_error(
                statement, error, VPS_ERROR_CLASS_SCHEMA, NULL);
        }
        for (index = 0U; index < statement->spec.parameter_count; ++index) {
            uint32_t expected = statement->parameter_types[index];
            uint32_t observed = client->api.result_parameter_type(
                client->api.context, result, (int)index);
            if (expected != 0U && observed != expected) {
                return vps_libpq_statement_error(
                    statement, error, VPS_ERROR_CLASS_SCHEMA, NULL);
            }
        }
    }
    if (statement->spec.discover_result_fields) return VPS_CLIENT_OK;
    for (index = 0U; index < statement->spec.result_field_count; ++index) {
        uint32_t observed_type = client->api.result_field_type(
            client->api.context, result, (int)index);
        if (observed_type != statement->spec.result_fields[index].type_oid) {
            return vps_libpq_statement_error(
                statement, error, VPS_ERROR_CLASS_METADATA, NULL);
        }
        if (client->api.result_field_format(client->api.context, result,
                                            (int)index) !=
            (int)statement->spec.result_fields[index].format) {
            return vps_libpq_statement_error(
                statement, error, VPS_ERROR_CLASS_CONVERSION, NULL);
        }
    }
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_libpq_statement_capture_metadata(
    VpsLibpqStatement *statement,
    const void *result,
    VpsError *error)
{
    VpsLibpqClient *client = statement->connection->client;
    int field_count = client->api.result_field_count(client->api.context,
                                                      result);
    size_t allocation_size;
    size_t index;
    if (!statement->spec.discover_result_fields) return VPS_CLIENT_OK;
    if (field_count <= 0 ||
        (size_t)field_count > VPS_CLIENT_MAX_RESULT_FIELD_COUNT ||
        vps_size_multiply((size_t)field_count,
                          sizeof(*statement->described_fields),
                          &allocation_size) != VPS_MEMORY_OK ||
        vps_memory_allocate(&client->allocator, allocation_size,
                            (void **)&statement->described_fields) !=
            VPS_MEMORY_OK) {
        return vps_libpq_statement_error(statement, error,
                                         VPS_ERROR_CLASS_MEMORY, NULL);
    }
    statement->described_fields_size = allocation_size;
    (void)memset(statement->described_fields, 0, allocation_size);
    for (index = 0U; index < (size_t)field_count; ++index) {
        VpsLibpqDescribedField *field = &statement->described_fields[index];
        const char *name = client->api.result_field_name(
            client->api.context, result, (int)index);
        size_t name_length = 0U;
        if (name == NULL) {
            return vps_libpq_statement_error(statement, error,
                                             VPS_ERROR_CLASS_METADATA, NULL);
        }
        while (name_length <= VPS_CLIENT_MAX_FIELD_NAME_BYTES &&
               name[name_length] != '\0') ++name_length;
        if (name_length == 0U ||
            name_length > VPS_CLIENT_MAX_FIELD_NAME_BYTES) {
            return vps_libpq_statement_error(statement, error,
                                             VPS_ERROR_CLASS_METADATA, NULL);
        }
        (void)memcpy(field->name, name, name_length + 1U);
        field->metadata.name = field->name;
        field->metadata.name_length = name_length;
        field->metadata.type_oid = client->api.result_field_type(
            client->api.context, result, (int)index);
        field->metadata.type_modifier = client->api.result_field_modifier(
            client->api.context, result, (int)index);
        field->metadata.origin_relation_oid =
            client->api.result_field_relation(client->api.context, result,
                                               (int)index);
        field->metadata.origin_attribute_number =
            client->api.result_field_attribute(client->api.context, result,
                                                (int)index);
        field->metadata.format = (VpsClientValueFormat)
            client->api.result_field_format(client->api.context, result,
                                            (int)index);
        if (field->metadata.type_oid == 0U ||
            (field->metadata.format != VPS_CLIENT_VALUE_TEXT &&
             field->metadata.format != VPS_CLIENT_VALUE_BINARY)) {
            return vps_libpq_statement_error(statement, error,
                                             VPS_ERROR_CLASS_METADATA, NULL);
        }
    }
    statement->described_field_count = (size_t)field_count;
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_libpq_statement_finish_result(
    VpsLibpqStatement *statement,
    int mode,
    VpsError *error)
{
    VpsLibpqClient *client = statement->connection->client;
    VpsLibpqResultStatus status;
    const char *sqlstate;
    VpsClientStatus check_result;
    VpsClientStatus failure_result;
    statement->current_result = client->api.get_result(
        client->api.context, statement->connection->postgresql_connection);
    if (statement->current_result == NULL) {
        return vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
    }
    status = client->api.result_status(client->api.context,
                                       statement->current_result);
    if ((mode != 2 && status != VPS_LIBPQ_RESULT_COMMAND_OK) ||
        (mode == 2 && status != VPS_LIBPQ_RESULT_TUPLES_OK &&
         status != VPS_LIBPQ_RESULT_COMMAND_OK)) {
        sqlstate = client->api.result_sqlstate(client->api.context,
                                               statement->current_result);
        vps_libpq_statement_log_server_error(
            statement, statement->current_result, sqlstate);
        vps_libpq_statement_log(statement, VPS_LOG_LEVEL_ERROR,
                                "libpq_statement", "result", "failed",
                                sqlstate);
        failure_result = vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_SQL, sqlstate);
        client->api.clear_result(client->api.context,
                                 statement->current_result);
        statement->current_result = NULL;
        return failure_result;
    }
    check_result = mode == 0
                       ? VPS_CLIENT_OK
                       : vps_libpq_statement_check_metadata(
                             statement, statement->current_result,
                             mode == 1, error);
    if (check_result == VPS_CLIENT_OK && mode == 1) {
        check_result = vps_libpq_statement_capture_metadata(
            statement, statement->current_result, error);
    }
    client->api.clear_result(client->api.context, statement->current_result);
    statement->current_result = NULL;
    if (check_result != VPS_CLIENT_OK) return check_result;
    statement->current_result = client->api.get_result(
        client->api.context, statement->connection->postgresql_connection);
    if (statement->current_result != NULL) {
        client->api.clear_result(client->api.context,
                                 statement->current_result);
        statement->current_result = NULL;
        return vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_INVARIANT, NULL);
    }
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_libpq_statement_fetch_result(
    VpsLibpqStatement *statement,
    VpsClientPollResult *result,
    VpsError *error)
{
    VpsLibpqClient *client = statement->connection->client;
    VpsLibpqResultStatus status;
    VpsClientStatus check_result;
    int row_count;
    size_t index;
    size_t row_bytes = 0U;
    statement->current_result = client->api.get_result(
        client->api.context, statement->connection->postgresql_connection);
    if (statement->current_result == NULL) {
        return vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_INVARIANT, NULL);
    }
    status = client->api.result_status(client->api.context,
                                       statement->current_result);
    if (status == VPS_LIBPQ_RESULT_SINGLE_TUPLE) {
        check_result = vps_libpq_statement_check_metadata(
            statement, statement->current_result, 0, error);
        row_count = client->api.result_row_count(
            client->api.context, statement->current_result);
        if (check_result != VPS_CLIENT_OK || row_count != 1) {
            client->api.clear_result(client->api.context,
                                     statement->current_result);
            statement->current_result = NULL;
            return check_result == VPS_CLIENT_OK
                       ? vps_libpq_statement_error(
                             statement, error, VPS_ERROR_CLASS_INVARIANT,
                             NULL)
                       : check_result;
        }
        for (index = 0U; index < statement->spec.result_field_count; ++index) {
            int length;
            if (client->api.result_value_is_null(
                    client->api.context, statement->current_result, 0,
                    (int)index)) continue;
            length = client->api.result_value_length(
                client->api.context, statement->current_result, 0,
                (int)index);
            if (length < 0 || (size_t)length > VPS_CLIENT_MAX_COLUMN_BYTES ||
                vps_size_add(row_bytes, (size_t)length, &row_bytes) !=
                    VPS_MEMORY_OK ||
                row_bytes > VPS_CLIENT_MAX_ROW_BYTES ||
                client->api.result_value(
                    client->api.context, statement->current_result, 0,
                    (int)index) == NULL) {
                client->api.clear_result(client->api.context,
                                         statement->current_result);
                statement->current_result = NULL;
                return vps_libpq_statement_error(
                    statement, error, VPS_ERROR_CLASS_CONVERSION, NULL);
            }
        }
        if (statement->published_row_count == UINT64_MAX ||
            UINT64_MAX - statement->published_byte_count <
                (uint64_t)row_bytes) {
            client->api.clear_result(client->api.context,
                                     statement->current_result);
            statement->current_result = NULL;
            return vps_libpq_statement_error(
                statement, error, VPS_ERROR_CLASS_CONFIG, NULL);
        }
        statement->published_row_count += 1U;
        statement->published_byte_count += (uint64_t)row_bytes;
        statement->current_row_bytes = row_bytes;
        statement->phase = VPS_LIBPQ_STATEMENT_ROW_READY;
        (void)memset(result, 0, sizeof(*result));
        result->outcome = VPS_CLIENT_POLL_ROW_READY;
        vps_libpq_statement_log(statement, VPS_LOG_LEVEL_DEBUG,
                                "libpq_statement", "fetch", "row", NULL);
        return VPS_CLIENT_OK;
    }
    if (status == VPS_LIBPQ_RESULT_TUPLES_OK) {
        const char *command_tuples;
        uint64_t affected = 0U;
        int affected_valid = 0;
        check_result = vps_libpq_statement_check_metadata(
            statement, statement->current_result, 0, error);
        row_count = client->api.result_row_count(
            client->api.context, statement->current_result);
        command_tuples = client->api.result_command_tuples(
            client->api.context, statement->current_result);
        if (command_tuples != NULL && command_tuples[0] != '\0') {
            const unsigned char *digit =
                (const unsigned char *)command_tuples;
            affected_valid = 1;
            while (*digit != '\0') {
                if (*digit < '0' || *digit > '9' ||
                    affected > (UINT64_MAX - (uint64_t)(*digit - '0')) / 10U) {
                    affected_valid = -1;
                    break;
                }
                affected = affected * 10U + (uint64_t)(*digit - '0');
                ++digit;
            }
        }
        client->api.clear_result(client->api.context,
                                 statement->current_result);
        statement->current_result = NULL;
        if (check_result != VPS_CLIENT_OK) return check_result;
        if (affected_valid < 0)
            return vps_libpq_statement_error(
                statement, error, VPS_ERROR_CLASS_INVARIANT, NULL);
        if (affected_valid > 0) {
            statement->affected_count = affected;
            statement->affected_count_valid = 1;
        }
        if (row_count != 0) {
            return vps_libpq_statement_error(
                statement, error, VPS_ERROR_CLASS_INVARIANT, NULL);
        }
        statement->current_result = client->api.get_result(
            client->api.context,
            statement->connection->postgresql_connection);
        if (statement->current_result != NULL) {
            client->api.clear_result(client->api.context,
                                     statement->current_result);
            statement->current_result = NULL;
            return vps_libpq_statement_error(
                statement, error, VPS_ERROR_CLASS_INVARIANT, NULL);
        }
        statement->phase = VPS_LIBPQ_STATEMENT_COMPLETE;
        (void)memset(result, 0, sizeof(*result));
        result->outcome = VPS_CLIENT_POLL_COMPLETE;
        vps_libpq_statement_log(statement, VPS_LOG_LEVEL_INFO,
                                "libpq_statement", "fetch", "complete",
                                NULL);
        return VPS_CLIENT_OK;
    }
    {
        const char *sqlstate = client->api.result_sqlstate(
            client->api.context, statement->current_result);
        vps_libpq_statement_log_server_error(
            statement, statement->current_result, sqlstate);
        VpsClientStatus failure = vps_libpq_statement_error(
            statement, error, VPS_ERROR_CLASS_SQL, sqlstate);
        vps_libpq_statement_log(statement, VPS_LOG_LEVEL_ERROR,
                                "libpq_statement", "fetch", "late_error",
                                sqlstate);
        client->api.clear_result(client->api.context,
                                 statement->current_result);
        statement->current_result = NULL;
        return failure;
    }
}

VpsClientStatus vps_libpq_statement_poll(
    void *context,
    void *statement_value,
    VpsClientPollResult *result,
    VpsError *error)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqStatement *statement = (VpsLibpqStatement *)statement_value;
    size_t internal_polls = 0U;
    VpsClientStatus operation_result;
    if (client == NULL || statement == NULL || result == NULL ||
        statement->connection->client != client ||
        statement->connection->phase != VPS_LIBPQ_PHASE_READY) {
        return VPS_CLIENT_INVALID_STATE;
    }
    operation_result = vps_libpq_statement_deadline(statement, error);
    if (operation_result != VPS_CLIENT_OK) return operation_result;
    for (;;) {
        int api_result;
        statement->poll_count += 1U;
        if (++internal_polls > VPS_LIBPQ_STATEMENT_MAX_INTERNAL_POLLS) {
            return vps_libpq_statement_wait_result(
                statement, result, VPS_CLIENT_WAIT_READ_WRITE,
                statement->phase == VPS_LIBPQ_STATEMENT_CANCEL_POLL ||
                        statement->phase == VPS_LIBPQ_STATEMENT_CANCEL_DRAIN
                    ? VPS_CLIENT_WAIT_CANCEL
                    : statement->phase <= VPS_LIBPQ_STATEMENT_DESCRIBE_RESULT
                    ? VPS_CLIENT_WAIT_PREPARE
                    : VPS_CLIENT_WAIT_EXECUTE);
        }
        switch (statement->phase) {
        case VPS_LIBPQ_STATEMENT_CANCEL_POLL:
        {
            VpsLibpqPollingStatus cancel_status = client->api.cancel_poll(
                client->api.context, statement->cancel_connection);
            if (cancel_status == VPS_LIBPQ_POLL_ACTIVE) {
                break;
            }
            if (cancel_status == VPS_LIBPQ_POLL_READING ||
                cancel_status == VPS_LIBPQ_POLL_WRITING) {
                return vps_libpq_statement_wait_result(
                    statement, result,
                    cancel_status == VPS_LIBPQ_POLL_READING
                        ? VPS_CLIENT_WAIT_READ
                        : VPS_CLIENT_WAIT_WRITE,
                    VPS_CLIENT_WAIT_CANCEL);
            }
            if (cancel_status != VPS_LIBPQ_POLL_OK) {
                return vps_libpq_statement_error(
                    statement, error, VPS_ERROR_CLASS_CANCEL, NULL);
            }
            client->api.cancel_finish(client->api.context,
                                      statement->cancel_connection);
            statement->cancel_connection = NULL;
            statement->phase = VPS_LIBPQ_STATEMENT_CANCEL_DRAIN;
            break;
        }
        case VPS_LIBPQ_STATEMENT_CANCEL_DRAIN:
        {
            void *cancelled_result;
            if (client->api.consume_input(
                    client->api.context,
                    statement->connection->postgresql_connection) != 1) {
                return vps_libpq_statement_error(
                    statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
            }
            if (client->api.is_busy(
                    client->api.context,
                    statement->connection->postgresql_connection)) {
                return vps_libpq_statement_wait_result(
                    statement, result, VPS_CLIENT_WAIT_READ,
                    VPS_CLIENT_WAIT_CANCEL);
            }
            cancelled_result = client->api.get_result(
                client->api.context,
                statement->connection->postgresql_connection);
            if (cancelled_result == NULL) {
                if (!statement->cancel_sqlstate_seen &&
                    !statement->cancel_terminal_seen) {
                    return vps_libpq_statement_error(
                        statement, error, VPS_ERROR_CLASS_INVARIANT, NULL);
                }
                if (statement->cancel_sqlstate_seen && error != NULL &&
                    error->initialized) {
                    (void)vps_error_set_sqlstate(
                        error, VPS_ERROR_OPERATION_CANCEL, "57014", 0, 0,
                        statement->query_fingerprint);
                }
                statement->phase = VPS_LIBPQ_STATEMENT_COMPLETE;
                (void)memset(result, 0, sizeof(*result));
                result->outcome = VPS_CLIENT_POLL_COMPLETE;
                vps_libpq_statement_log(statement, VPS_LOG_LEVEL_INFO,
                                        "libpq_statement", "cancel",
                                        statement->cancel_sqlstate_seen
                                            ? "drained"
                                            : "already_complete",
                                        statement->cancel_sqlstate_seen
                                            ? "57014"
                                            : NULL);
                return VPS_CLIENT_OK;
            }
            if (client->api.result_status(client->api.context,
                                          cancelled_result) ==
                    VPS_LIBPQ_RESULT_FATAL_ERROR) {
                const char *sqlstate = client->api.result_sqlstate(
                    client->api.context, cancelled_result);
                vps_libpq_statement_log_server_error(
                    statement, cancelled_result, sqlstate);
                if (sqlstate != NULL && strcmp(sqlstate, "57014") == 0) {
                    statement->cancel_sqlstate_seen = 1;
                }
            } else if (client->api.result_status(client->api.context,
                                                 cancelled_result) ==
                       VPS_LIBPQ_RESULT_TUPLES_OK) {
                statement->cancel_terminal_seen = 1;
            }
            client->api.clear_result(client->api.context, cancelled_result);
            break;
        }
        case VPS_LIBPQ_STATEMENT_PREPARE_FLUSH:
        case VPS_LIBPQ_STATEMENT_DESCRIBE_FLUSH:
        case VPS_LIBPQ_STATEMENT_EXECUTE_FLUSH:
            api_result = client->api.flush(
                client->api.context,
                statement->connection->postgresql_connection);
            if (api_result < 0) {
                return vps_libpq_statement_error(
                    statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
            }
            if (api_result == 1) {
                return vps_libpq_statement_wait_result(
                    statement, result, VPS_CLIENT_WAIT_WRITE,
                    statement->phase == VPS_LIBPQ_STATEMENT_EXECUTE_FLUSH
                        ? VPS_CLIENT_WAIT_EXECUTE
                        : VPS_CLIENT_WAIT_PREPARE);
            }
            if (statement->phase == VPS_LIBPQ_STATEMENT_PREPARE_FLUSH) {
                statement->phase = VPS_LIBPQ_STATEMENT_PREPARE_WAIT;
            } else if (statement->phase ==
                       VPS_LIBPQ_STATEMENT_DESCRIBE_FLUSH) {
                statement->phase = VPS_LIBPQ_STATEMENT_DESCRIBE_WAIT;
            } else if (statement->spec.single_row) {
                statement->phase = VPS_LIBPQ_STATEMENT_FETCH_WAIT;
                (void)memset(result, 0, sizeof(*result));
                result->outcome = VPS_CLIENT_POLL_COMPLETE;
                return VPS_CLIENT_OK;
            } else {
                statement->phase = VPS_LIBPQ_STATEMENT_EXECUTE_WAIT;
            }
            break;
        case VPS_LIBPQ_STATEMENT_FETCH_WAIT:
            if (client->api.consume_input(
                    client->api.context,
                    statement->connection->postgresql_connection) != 1) {
                return vps_libpq_statement_error(
                    statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
            }
            if (client->api.is_busy(
                    client->api.context,
                    statement->connection->postgresql_connection)) {
                return vps_libpq_statement_wait_result(
                    statement, result, VPS_CLIENT_WAIT_READ,
                    VPS_CLIENT_WAIT_FETCH);
            }
            return vps_libpq_statement_fetch_result(statement, result,
                                                     error);
        case VPS_LIBPQ_STATEMENT_PREPARE_WAIT:
        case VPS_LIBPQ_STATEMENT_DESCRIBE_WAIT:
        case VPS_LIBPQ_STATEMENT_EXECUTE_WAIT:
            if (client->api.consume_input(
                    client->api.context,
                    statement->connection->postgresql_connection) != 1) {
                return vps_libpq_statement_error(
                    statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
            }
            if (client->api.is_busy(
                    client->api.context,
                    statement->connection->postgresql_connection)) {
                return vps_libpq_statement_wait_result(
                    statement, result, VPS_CLIENT_WAIT_READ,
                    statement->phase == VPS_LIBPQ_STATEMENT_EXECUTE_WAIT
                        ? VPS_CLIENT_WAIT_EXECUTE
                        : VPS_CLIENT_WAIT_PREPARE);
            }
            if (statement->phase == VPS_LIBPQ_STATEMENT_PREPARE_WAIT) {
                statement->phase = VPS_LIBPQ_STATEMENT_PREPARE_RESULT;
            } else if (statement->phase ==
                       VPS_LIBPQ_STATEMENT_DESCRIBE_WAIT) {
                statement->phase = VPS_LIBPQ_STATEMENT_DESCRIBE_RESULT;
            } else {
                statement->phase = VPS_LIBPQ_STATEMENT_EXECUTE_RESULT;
            }
            break;
        case VPS_LIBPQ_STATEMENT_PREPARE_RESULT:
            operation_result = vps_libpq_statement_finish_result(
                statement, 0, error);
            if (operation_result != VPS_CLIENT_OK) return operation_result;
            if (client->api.send_describe_prepared(
                    client->api.context,
                    statement->connection->postgresql_connection,
                    statement->prepared_name) != 1) {
                return vps_libpq_statement_error(
                    statement, error, VPS_ERROR_CLASS_CONNECTION, NULL);
            }
            statement->phase = VPS_LIBPQ_STATEMENT_DESCRIBE_FLUSH;
            break;
        case VPS_LIBPQ_STATEMENT_DESCRIBE_RESULT:
            operation_result = vps_libpq_statement_finish_result(
                statement, 1, error);
            if (operation_result != VPS_CLIENT_OK) return operation_result;
            statement->described = 1;
            statement->phase = VPS_LIBPQ_STATEMENT_PREPARED;
            (void)memset(result, 0, sizeof(*result));
            result->outcome = VPS_CLIENT_POLL_COMPLETE;
            vps_libpq_statement_log(statement, VPS_LOG_LEVEL_INFO,
                                    "libpq_statement", "describe",
                                    "complete", NULL);
            return VPS_CLIENT_OK;
        case VPS_LIBPQ_STATEMENT_EXECUTE_RESULT:
            operation_result = vps_libpq_statement_finish_result(
                statement, 2, error);
            if (operation_result != VPS_CLIENT_OK) return operation_result;
            statement->described = 1;
            statement->phase = VPS_LIBPQ_STATEMENT_COMPLETE;
            (void)memset(result, 0, sizeof(*result));
            result->outcome = VPS_CLIENT_POLL_COMPLETE;
            vps_libpq_statement_log(statement, VPS_LOG_LEVEL_INFO,
                                    "libpq_statement", "execute",
                                    "complete", NULL);
            return VPS_CLIENT_OK;
        default:
            return VPS_CLIENT_INVALID_STATE;
        }
        operation_result = vps_libpq_statement_deadline(statement, error);
        if (operation_result != VPS_CLIENT_OK) return operation_result;
    }
}

VpsClientStatus vps_libpq_statement_wait(
    void *context,
    void *statement_value,
    const VpsClientWaitRequest *wait_request,
    VpsError *error)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqStatement *statement = (VpsLibpqStatement *)statement_value;
    VpsSocketWaitRequest request;
    VpsSocketWaitResult wait_result;
    VpsDeadlineStatus deadline_status;
    if (client == NULL || statement == NULL || wait_request == NULL ||
        statement->connection->client != client ||
        statement->connection->phase != VPS_LIBPQ_PHASE_READY ||
        wait_request->interest != statement->wait_interest ||
        wait_request->phase != statement->wait_phase) {
        return VPS_CLIENT_INVALID_STATE;
    }
    (void)memset(&request, 0, sizeof(request));
    request.operations = client->platform_operations;
    request.socket_handle = statement->socket_handle;
    request.interest = (VpsWaitInterest)wait_request->interest;
    request.deadline = &statement->deadline;
    request.max_slice_ms = wait_request->max_slice_ms;
    if (wait_request->phase != VPS_CLIENT_WAIT_CANCEL) {
        request.interrupt_probe = statement->spec.interrupt_probe != NULL
                                      ? statement->spec.interrupt_probe
                                      : client->interrupt_probe;
        request.interrupt_context = statement->spec.interrupt_probe != NULL
                                        ? statement->spec.interrupt_context
                                        : client->interrupt_context;
    }
    request.logger = client->logger;
    request.phase = wait_request->phase == VPS_CLIENT_WAIT_CANCEL
                        ? VPS_WAIT_PHASE_CANCEL
                        : VPS_WAIT_PHASE_STATEMENT;
    deadline_status = vps_socket_wait_execute(&request, &wait_result);
    statement->wait_count += wait_result.wait_count;
    if (deadline_status == VPS_DEADLINE_OK &&
        wait_result.outcome == VPS_SOCKET_WAIT_READY) return VPS_CLIENT_OK;
    if (deadline_status == VPS_DEADLINE_OK &&
        wait_result.outcome == VPS_SOCKET_WAIT_DEADLINE_EXPIRED) {
        return vps_libpq_statement_control_signal(
            statement, error, VPS_ERROR_CLASS_TIMEOUT);
    }
    if (deadline_status == VPS_DEADLINE_OK &&
        wait_result.outcome == VPS_SOCKET_WAIT_INTERRUPTED) {
        return vps_libpq_statement_control_signal(
            statement, error, VPS_ERROR_CLASS_CANCEL);
    }
    return vps_libpq_statement_error(statement, error,
                                     VPS_ERROR_CLASS_CONNECTION, NULL);
}

VpsClientStatus vps_libpq_statement_metadata(
    void *context,
    const void *statement_value,
    VpsClientStatementMetadata *metadata,
    VpsError *error)
{
    const VpsLibpqClient *client = (const VpsLibpqClient *)context;
    const VpsLibpqStatement *statement =
        (const VpsLibpqStatement *)statement_value;
    (void)error;
    if (client == NULL || statement == NULL || metadata == NULL ||
        statement->connection->client != client ||
        (statement->spec.discover_result_fields && !statement->described)) {
        return VPS_CLIENT_INVALID_STATE;
    }
    (void)memset(metadata, 0, sizeof(*metadata));
    metadata->parameter_count = statement->spec.parameter_count;
    metadata->result_field_count = statement->spec.result_field_count;
    if (statement->spec.discover_result_fields) {
        metadata->result_field_count = statement->described_field_count;
    }
    metadata->query_fingerprint = statement->query_hash;
    metadata->published_row_count = statement->published_row_count;
    metadata->affected_count = statement->affected_count;
    metadata->affected_count_valid = statement->affected_count_valid;
    metadata->described = statement->described ||
                          !statement->spec.discover_result_fields;
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_libpq_statement_result_field(
    void *context,
    const void *statement_value,
    size_t field_index,
    VpsClientResultFieldMetadata *field,
    VpsError *error)
{
    const VpsLibpqClient *client = (const VpsLibpqClient *)context;
    const VpsLibpqStatement *statement =
        (const VpsLibpqStatement *)statement_value;
    (void)error;
    if (client == NULL || statement == NULL || field == NULL ||
        statement->connection->client != client || !statement->described ||
        !statement->spec.discover_result_fields ||
        field_index >= statement->described_field_count) {
        return VPS_CLIENT_INVALID_ARGUMENT;
    }
    *field = statement->described_fields[field_index].metadata;
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_libpq_statement_row(
    void *context,
    const void *statement_value,
    size_t *column_count,
    VpsError *error)
{
    const VpsLibpqClient *client = (const VpsLibpqClient *)context;
    const VpsLibpqStatement *statement =
        (const VpsLibpqStatement *)statement_value;
    (void)error;
    if (client == NULL || statement == NULL || column_count == NULL ||
        statement->connection->client != client ||
        statement->phase != VPS_LIBPQ_STATEMENT_ROW_READY ||
        statement->current_result == NULL) return VPS_CLIENT_INVALID_STATE;
    *column_count = statement->spec.result_field_count;
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_libpq_statement_column(
    void *context,
    const void *statement_value,
    size_t column_index,
    VpsClientColumnView *column,
    VpsError *error)
{
    const VpsLibpqClient *client = (const VpsLibpqClient *)context;
    const VpsLibpqStatement *statement =
        (const VpsLibpqStatement *)statement_value;
    int is_null;
    int length;
    (void)error;
    if (client == NULL || statement == NULL || column == NULL ||
        statement->connection->client != client ||
        statement->phase != VPS_LIBPQ_STATEMENT_ROW_READY ||
        statement->current_result == NULL ||
        column_index >= statement->spec.result_field_count) {
        return VPS_CLIENT_INVALID_ARGUMENT;
    }
    (void)memset(column, 0, sizeof(*column));
    is_null = client->api.result_value_is_null(
        client->api.context, statement->current_result, 0,
        (int)column_index);
    column->type_oid = client->api.result_field_type(
        client->api.context, statement->current_result, (int)column_index);
    column->format = (VpsClientValueFormat)client->api.result_field_format(
        client->api.context, statement->current_result, (int)column_index);
    column->is_null = is_null != 0;
    if (column->is_null) return VPS_CLIENT_OK;
    length = client->api.result_value_length(
        client->api.context, statement->current_result, 0,
        (int)column_index);
    if (length < 0 || (size_t)length > VPS_CLIENT_MAX_COLUMN_BYTES) {
        return VPS_CLIENT_LIMIT_EXCEEDED;
    }
    column->data = client->api.result_value(
        client->api.context, statement->current_result, 0,
        (int)column_index);
    if (column->data == NULL) return VPS_CLIENT_BACKEND_ERROR;
    column->length = (size_t)length;
    return VPS_CLIENT_OK;
}

void vps_libpq_statement_row_release(void *context, void *statement_value)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqStatement *statement = (VpsLibpqStatement *)statement_value;
    if (client == NULL || statement == NULL ||
        statement->connection->client != client ||
        statement->phase != VPS_LIBPQ_STATEMENT_ROW_READY ||
        statement->current_result == NULL) return;
    client->api.clear_result(client->api.context,
                             statement->current_result);
    statement->current_result = NULL;
    statement->current_row_bytes = 0U;
    statement->phase = VPS_LIBPQ_STATEMENT_FETCH_WAIT;
}

void vps_libpq_statement_destroy(void *context, void *statement_value)
{
    VpsLibpqClient *client = (VpsLibpqClient *)context;
    VpsLibpqStatement *statement = (VpsLibpqStatement *)statement_value;
    if (client == NULL || statement == NULL ||
        statement->connection->client != client) return;
    if (statement->current_result != NULL) {
        client->api.clear_result(client->api.context,
                                 statement->current_result);
        statement->current_result = NULL;
    }
    if (statement->cancel_connection != NULL) {
        client->api.cancel_finish(client->api.context,
                                  statement->cancel_connection);
        statement->cancel_connection = NULL;
    }
    if (statement->phase != VPS_LIBPQ_STATEMENT_CREATED &&
        statement->phase != VPS_LIBPQ_STATEMENT_PREPARED &&
        statement->phase != VPS_LIBPQ_STATEMENT_COMPLETE &&
        statement->phase != VPS_LIBPQ_STATEMENT_FAILED) {
        statement->connection->phase = VPS_LIBPQ_PHASE_FAILED;
    }
    if (statement->connection->statement_count != 0U) {
        statement->connection->statement_count -= 1U;
    }
    vps_libpq_statement_release_arrays(statement);
    vps_memory_release(&client->allocator, (void **)&statement,
                       sizeof(*statement));
}
