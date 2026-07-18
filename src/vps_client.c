#include "vps_client.h"

#include <limits.h>
#include <string.h>

struct VpsClientConnection {
    VpsClient *client;
    void *backend_connection;
    VpsClientConnectionState state;
    VpsClientOperation active_operation;
    size_t statement_count;
};

struct VpsClientRowView {
    VpsClientStatement *statement;
    uint64_t generation;
    size_t column_count;
};

struct VpsClientStatement {
    VpsClientConnection *connection;
    void *backend_statement;
    VpsClientStatementState state;
    VpsClientOperation active_operation;
    uint64_t row_generation;
    VpsClientRowView row_view;
};

static int vps_client_status_valid(VpsClientStatus status)
{
    return status >= VPS_CLIENT_OK && status <= VPS_CLIENT_BACKEND_ERROR;
}

static int vps_client_operation_valid(VpsClientOperation operation)
{
    return operation >= VPS_CLIENT_OPERATION_CONNECT &&
           operation <= VPS_CLIENT_OPERATION_CANCEL;
}

static VpsErrorOperation vps_client_error_operation(
    VpsClientOperation operation)
{
    switch (operation) {
    case VPS_CLIENT_OPERATION_CONNECT:
        return VPS_ERROR_OPERATION_CONNECT;
    case VPS_CLIENT_OPERATION_COMMIT:
        return VPS_ERROR_OPERATION_COMMIT;
    case VPS_CLIENT_OPERATION_ROLLBACK:
        return VPS_ERROR_OPERATION_ROLLBACK;
    case VPS_CLIENT_OPERATION_CANCEL:
        return VPS_ERROR_OPERATION_CANCEL;
    default:
        return VPS_ERROR_OPERATION_QUERY;
    }
}

static VpsErrorClass vps_client_error_class(VpsClientStatus status)
{
    switch (status) {
    case VPS_CLIENT_UNSUPPORTED:
        return VPS_ERROR_CLASS_UNSUPPORTED;
    case VPS_CLIENT_OUT_OF_MEMORY:
        return VPS_ERROR_CLASS_MEMORY;
    case VPS_CLIENT_LIMIT_EXCEEDED:
        return VPS_ERROR_CLASS_CONFIG;
    case VPS_CLIENT_BACKEND_ERROR:
        return VPS_ERROR_CLASS_CONNECTION;
    default:
        return VPS_ERROR_CLASS_INVARIANT;
    }
}

static VpsClientStatus vps_client_fail(VpsError *error,
                                       VpsClientOperation operation,
                                       VpsClientStatus status)
{
    if (error != NULL && error->initialized) {
        VpsMemoryResult error_result = vps_error_set_local(
            error, vps_client_error_operation(operation),
            vps_client_error_class(status), NULL);
        if (error_result != VPS_MEMORY_OK) return VPS_CLIENT_OUT_OF_MEMORY;
    }
    return status;
}

static void vps_client_log(VpsClient *client,
                           VpsLogLevel level,
                           VpsClientOperation operation,
                           const char *phase,
                           const char *status)
{
    VpsLogEvent event;
    const char *operation_name;
    if (client == NULL || client->logger == NULL || phase == NULL ||
        status == NULL) return;
    operation_name = vps_client_operation_name(operation);
    if (vps_log_event_init(&event, level) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION,
                                 operation_name,
                                 strlen(operation_name)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 strlen(phase)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK) return;
    vps_logger_emit(client->logger, &event);
}

static uint64_t vps_client_operation_capability(VpsClientOperation operation)
{
    switch (operation) {
    case VPS_CLIENT_OPERATION_CONNECT: return VPS_CLIENT_CAP_CONNECT;
    case VPS_CLIENT_OPERATION_PREPARE: return VPS_CLIENT_CAP_PREPARE;
    case VPS_CLIENT_OPERATION_EXECUTE: return VPS_CLIENT_CAP_EXECUTE;
    case VPS_CLIENT_OPERATION_FETCH: return VPS_CLIENT_CAP_FETCH;
    case VPS_CLIENT_OPERATION_BEGIN:
    case VPS_CLIENT_OPERATION_COMMIT:
    case VPS_CLIENT_OPERATION_ROLLBACK:
        return VPS_CLIENT_CAP_TRANSACTION;
    case VPS_CLIENT_OPERATION_RESET: return VPS_CLIENT_CAP_RESET;
    case VPS_CLIENT_OPERATION_PING: return VPS_CLIENT_CAP_PING;
    case VPS_CLIENT_OPERATION_CANCEL: return VPS_CLIENT_CAP_CANCEL;
    default: return UINT64_C(0);
    }
}

static VpsClientWaitPhase vps_client_operation_wait_phase(
    VpsClientOperation operation)
{
    switch (operation) {
    case VPS_CLIENT_OPERATION_CONNECT: return VPS_CLIENT_WAIT_CONNECT;
    case VPS_CLIENT_OPERATION_PREPARE: return VPS_CLIENT_WAIT_PREPARE;
    case VPS_CLIENT_OPERATION_EXECUTE: return VPS_CLIENT_WAIT_EXECUTE;
    case VPS_CLIENT_OPERATION_FETCH: return VPS_CLIENT_WAIT_FETCH;
    case VPS_CLIENT_OPERATION_BEGIN:
    case VPS_CLIENT_OPERATION_COMMIT:
    case VPS_CLIENT_OPERATION_ROLLBACK:
        return VPS_CLIENT_WAIT_TRANSACTION;
    case VPS_CLIENT_OPERATION_RESET: return VPS_CLIENT_WAIT_RESET;
    case VPS_CLIENT_OPERATION_PING: return VPS_CLIENT_WAIT_PING;
    case VPS_CLIENT_OPERATION_CANCEL: return VPS_CLIENT_WAIT_CANCEL;
    default: return VPS_CLIENT_WAIT_CONNECT;
    }
}

static int vps_client_wait_valid(const VpsClientWaitRequest *wait,
                                 VpsClientOperation operation)
{
    return wait != NULL &&
           wait->interest >= VPS_CLIENT_WAIT_READ &&
           wait->interest <= VPS_CLIENT_WAIT_READ_WRITE &&
           wait->phase == vps_client_operation_wait_phase(operation) &&
           wait->max_slice_ms > 0U &&
           wait->max_slice_ms <= VPS_CLIENT_WAIT_MAX_SLICE_MS;
}

static int vps_client_poll_outcome_valid(VpsClientPollOutcome outcome)
{
    return outcome >= VPS_CLIENT_POLL_WAIT &&
           outcome <= VPS_CLIENT_POLL_FAILED;
}

static int vps_client_operations_valid(const VpsClientOperations *operations)
{
    return operations != NULL &&
           operations->structure_size >= sizeof(VpsClientOperations) &&
           operations->contract_version == VPS_CLIENT_CONTRACT_VERSION &&
           (operations->capabilities & ~VPS_CLIENT_CAP_ALL) == 0U &&
           operations->connection_create != NULL &&
           operations->connection_start != NULL &&
           operations->connection_poll != NULL &&
           operations->connection_wait != NULL &&
           operations->connection_destroy != NULL &&
           operations->statement_create != NULL &&
           operations->statement_start != NULL &&
           operations->statement_poll != NULL &&
           operations->statement_wait != NULL &&
           operations->statement_metadata != NULL &&
           operations->statement_result_field != NULL &&
           operations->statement_row != NULL &&
           operations->statement_column != NULL &&
           operations->statement_row_release != NULL &&
           operations->statement_destroy != NULL;
}

static int vps_client_statement_spec_valid(
    const VpsClientStatementSpec *spec)
{
    size_t index;
    if (spec == NULL || spec->query == NULL || spec->query_length == 0U ||
        spec->query_length > VPS_CLIENT_MAX_QUERY_BYTES ||
        spec->query[spec->query_length] != '\0' ||
        spec->parameter_count > VPS_CLIENT_MAX_PARAMETER_COUNT ||
        (spec->parameter_count != 0U && spec->parameters == NULL) ||
        spec->result_field_count > VPS_CLIENT_MAX_RESULT_FIELD_COUNT ||
        (spec->result_field_count != 0U && spec->result_fields == NULL) ||
        spec->timeout_ms == 0U ||
        spec->timeout_ms > VPS_CLIENT_MAX_STATEMENT_TIMEOUT_MS ||
        (spec->prepare != 0 && spec->prepare != 1) ||
        (spec->single_row != 0 && spec->single_row != 1) ||
        (spec->discover_result_fields != 0 &&
         spec->discover_result_fields != 1) ||
        (spec->discover_result_fields &&
         (!spec->prepare || spec->single_row ||
          spec->result_field_count != 0U))) return 0;
    for (index = 0U; index < spec->query_length; ++index) {
        if (spec->query[index] == '\0') return 0;
    }
    for (index = 0U; index < spec->parameter_count; ++index) {
        const VpsClientParameterView *parameter = &spec->parameters[index];
        if ((parameter->format != VPS_CLIENT_VALUE_TEXT &&
             parameter->format != VPS_CLIENT_VALUE_BINARY) ||
            (parameter->is_null != 0 && parameter->is_null != 1) ||
            parameter->length > (size_t)INT_MAX ||
            (parameter->is_null &&
             (parameter->value != NULL || parameter->length != 0U)) ||
            (!parameter->is_null && parameter->value == NULL)) return 0;
    }
    for (index = 0U; index < spec->result_field_count; ++index) {
        if (spec->result_fields[index].type_oid == 0U ||
            (spec->result_fields[index].format != VPS_CLIENT_VALUE_TEXT &&
             spec->result_fields[index].format != VPS_CLIENT_VALUE_BINARY)) {
            return 0;
        }
        if (index != 0U &&
            spec->result_fields[index].format !=
                spec->result_fields[0].format) return 0;
    }
    return 1;
}

VpsClientStatus vps_client_init(VpsClient *client,
                                const VpsAllocator *allocator,
                                const VpsClientOperations *operations,
                                void *backend_context,
                                VpsLogger *logger)
{
    if (client == NULL || !vps_allocator_is_valid(allocator) ||
        !vps_client_operations_valid(operations)) {
        return VPS_CLIENT_INVALID_ARGUMENT;
    }
    (void)memset(client, 0, sizeof(*client));
    client->allocator = *allocator;
    (void)memcpy(&client->operations, operations,
                 sizeof(client->operations));
    client->operations.structure_size = sizeof(client->operations);
    client->backend_context = backend_context;
    client->logger = logger;
    client->initialized = 1;
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_client_cleanup(VpsClient *client)
{
    if (client == NULL) return VPS_CLIENT_INVALID_ARGUMENT;
    if (!client->initialized) return VPS_CLIENT_OK;
    if (client->connection_count != 0U) return VPS_CLIENT_INVALID_STATE;
    (void)memset(client, 0, sizeof(*client));
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_client_connection_open(
    VpsClient *client,
    VpsClientConnection **connection,
    VpsError *error)
{
    VpsClientConnection *replacement = NULL;
    VpsClientStatus result;
    if (client == NULL || !client->initialized || connection == NULL ||
        *connection != NULL) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_CONNECT,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    if (vps_memory_allocate(&client->allocator, sizeof(*replacement),
                            (void **)&replacement) != VPS_MEMORY_OK) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_CONNECT,
                               VPS_CLIENT_OUT_OF_MEMORY);
    }
    (void)memset(replacement, 0, sizeof(*replacement));
    replacement->client = client;
    replacement->state = VPS_CLIENT_CONNECTION_NEW;
    result = client->operations.connection_create(
        client->backend_context, &replacement->backend_connection, error);
    if (!vps_client_status_valid(result)) result = VPS_CLIENT_BACKEND_ERROR;
    if (result != VPS_CLIENT_OK || replacement->backend_connection == NULL) {
        if (replacement->backend_connection != NULL) {
            client->operations.connection_destroy(
                client->backend_context, replacement->backend_connection);
            replacement->backend_connection = NULL;
        }
        vps_memory_release(&client->allocator, (void **)&replacement,
                           sizeof(*replacement));
        result = result == VPS_CLIENT_OK ? VPS_CLIENT_BACKEND_ERROR : result;
        vps_client_log(client, VPS_LOG_LEVEL_ERROR,
                       VPS_CLIENT_OPERATION_CONNECT, "open",
                       vps_client_status_name(result));
        return result;
    }
    client->connection_count += 1U;
    *connection = replacement;
    vps_client_log(client, VPS_LOG_LEVEL_DEBUG,
                   VPS_CLIENT_OPERATION_CONNECT, "open", "ok");
    return VPS_CLIENT_OK;
}

static int vps_client_connection_start_allowed(
    const VpsClientConnection *connection,
    VpsClientOperation operation)
{
    if (connection->active_operation != VPS_CLIENT_OPERATION_NONE) {
        return operation == VPS_CLIENT_OPERATION_CANCEL &&
               connection->state == VPS_CLIENT_CONNECTION_CONNECTING;
    }
    switch (operation) {
    case VPS_CLIENT_OPERATION_CONNECT:
        return connection->state == VPS_CLIENT_CONNECTION_NEW;
    case VPS_CLIENT_OPERATION_BEGIN:
    case VPS_CLIENT_OPERATION_RESET:
    case VPS_CLIENT_OPERATION_PING:
        return connection->state == VPS_CLIENT_CONNECTION_READY;
    case VPS_CLIENT_OPERATION_COMMIT:
    case VPS_CLIENT_OPERATION_ROLLBACK:
        return connection->state ==
               VPS_CLIENT_CONNECTION_TRANSACTION_ACTIVE;
    default:
        return 0;
    }
}

static VpsClientConnectionState vps_client_connection_running_state(
    VpsClientOperation operation)
{
    switch (operation) {
    case VPS_CLIENT_OPERATION_CONNECT:
        return VPS_CLIENT_CONNECTION_CONNECTING;
    case VPS_CLIENT_OPERATION_BEGIN:
        return VPS_CLIENT_CONNECTION_TRANSACTION_STARTING;
    case VPS_CLIENT_OPERATION_COMMIT:
    case VPS_CLIENT_OPERATION_ROLLBACK:
        return VPS_CLIENT_CONNECTION_TRANSACTION_ENDING;
    case VPS_CLIENT_OPERATION_RESET:
        return VPS_CLIENT_CONNECTION_RESETTING;
    case VPS_CLIENT_OPERATION_PING:
        return VPS_CLIENT_CONNECTION_PINGING;
    case VPS_CLIENT_OPERATION_CANCEL:
        return VPS_CLIENT_CONNECTION_CANCELLING;
    default:
        return VPS_CLIENT_CONNECTION_FAILED;
    }
}

static VpsClientConnectionState vps_client_connection_terminal_state(
    VpsClientOperation operation)
{
    switch (operation) {
    case VPS_CLIENT_OPERATION_CONNECT:
    case VPS_CLIENT_OPERATION_COMMIT:
    case VPS_CLIENT_OPERATION_ROLLBACK:
    case VPS_CLIENT_OPERATION_RESET:
    case VPS_CLIENT_OPERATION_PING:
        return VPS_CLIENT_CONNECTION_READY;
    case VPS_CLIENT_OPERATION_BEGIN:
        return VPS_CLIENT_CONNECTION_TRANSACTION_ACTIVE;
    default:
        return VPS_CLIENT_CONNECTION_FAILED;
    }
}

VpsClientStatus vps_client_connection_start(
    VpsClientConnection *connection,
    VpsClientOperation operation,
    VpsError *error)
{
    VpsClient *client;
    VpsClientStatus result;
    if (connection == NULL || connection->client == NULL ||
        !vps_client_operation_valid(operation)) {
        return vps_client_fail(error, operation, VPS_CLIENT_INVALID_ARGUMENT);
    }
    client = connection->client;
    if ((client->operations.capabilities &
         vps_client_operation_capability(operation)) == 0U) {
        return vps_client_fail(error, operation, VPS_CLIENT_UNSUPPORTED);
    }
    if (connection->statement_count != 0U ||
        !vps_client_connection_start_allowed(connection, operation)) {
        return vps_client_fail(error, operation, VPS_CLIENT_INVALID_STATE);
    }
    result = client->operations.connection_start(
        client->backend_context, connection->backend_connection, operation,
        error);
    if (!vps_client_status_valid(result)) result = VPS_CLIENT_BACKEND_ERROR;
    if (result != VPS_CLIENT_OK) {
        connection->state = VPS_CLIENT_CONNECTION_FAILED;
        connection->active_operation = VPS_CLIENT_OPERATION_NONE;
        vps_client_log(client, VPS_LOG_LEVEL_ERROR, operation, "start",
                       vps_client_status_name(result));
        return result;
    }
    connection->active_operation = operation;
    connection->state = vps_client_connection_running_state(operation);
    vps_client_log(client, VPS_LOG_LEVEL_DEBUG, operation,
                   vps_client_connection_state_name(connection->state),
                   "started");
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_client_connection_poll(
    VpsClientConnection *connection,
    VpsClientPollResult *poll_result,
    VpsError *error)
{
    VpsClient *client;
    VpsClientPollResult observed;
    VpsClientOperation operation;
    VpsClientStatus result;
    if (connection == NULL || connection->client == NULL ||
        poll_result == NULL) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_NONE,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    (void)memset(poll_result, 0, sizeof(*poll_result));
    operation = connection->active_operation;
    if (!vps_client_operation_valid(operation)) {
        return vps_client_fail(error, operation, VPS_CLIENT_INVALID_STATE);
    }
    client = connection->client;
    (void)memset(&observed, 0, sizeof(observed));
    observed.outcome = (VpsClientPollOutcome)-1;
    result = client->operations.connection_poll(
        client->backend_context, connection->backend_connection, &observed,
        error);
    if (!vps_client_status_valid(result)) result = VPS_CLIENT_BACKEND_ERROR;
    if (result != VPS_CLIENT_OK ||
        !vps_client_poll_outcome_valid(observed.outcome) ||
        observed.outcome == VPS_CLIENT_POLL_ROW_READY ||
        (observed.outcome == VPS_CLIENT_POLL_WAIT &&
         !vps_client_wait_valid(&observed.wait, operation))) {
        result = result == VPS_CLIENT_OK ? VPS_CLIENT_BACKEND_ERROR : result;
        connection->state = VPS_CLIENT_CONNECTION_FAILED;
        connection->active_operation = VPS_CLIENT_OPERATION_NONE;
        vps_client_log(client, VPS_LOG_LEVEL_ERROR, operation, "poll",
                       vps_client_status_name(result));
        return result;
    }
    *poll_result = observed;
    if (observed.outcome == VPS_CLIENT_POLL_WAIT) {
        vps_client_log(client, VPS_LOG_LEVEL_DEBUG, operation, "poll",
                       "wait");
        return VPS_CLIENT_OK;
    }
    connection->active_operation = VPS_CLIENT_OPERATION_NONE;
    if (observed.outcome == VPS_CLIENT_POLL_FAILED) {
        connection->state = VPS_CLIENT_CONNECTION_FAILED;
        vps_client_log(client, VPS_LOG_LEVEL_ERROR, operation, "poll",
                       "failed");
        return VPS_CLIENT_BACKEND_ERROR;
    }
    connection->state = vps_client_connection_terminal_state(operation);
    vps_client_log(client, VPS_LOG_LEVEL_DEBUG, operation,
                   vps_client_connection_state_name(connection->state),
                   "complete");
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_client_connection_wait(
    VpsClientConnection *connection,
    const VpsClientWaitRequest *wait_request,
    VpsError *error)
{
    VpsClientOperation operation;
    VpsClientStatus result;
    if (connection == NULL || connection->client == NULL ||
        wait_request == NULL) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_NONE,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    operation = connection->active_operation;
    if (!vps_client_operation_valid(operation) ||
        !vps_client_wait_valid(wait_request, operation)) {
        return vps_client_fail(error, operation, VPS_CLIENT_INVALID_STATE);
    }
    result = connection->client->operations.connection_wait(
        connection->client->backend_context, connection->backend_connection,
        wait_request, error);
    if (!vps_client_status_valid(result)) result = VPS_CLIENT_BACKEND_ERROR;
    if (result != VPS_CLIENT_OK) {
        connection->state = VPS_CLIENT_CONNECTION_FAILED;
        connection->active_operation = VPS_CLIENT_OPERATION_NONE;
        vps_client_log(connection->client, VPS_LOG_LEVEL_ERROR, operation,
                       "wait", vps_client_status_name(result));
        return result;
    }
    vps_client_log(connection->client, VPS_LOG_LEVEL_DEBUG, operation,
                   "wait", "ready");
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_client_connection_close(
    VpsClientConnection **connection)
{
    VpsClientConnection *owned;
    VpsClient *client;
    if (connection == NULL) return VPS_CLIENT_INVALID_ARGUMENT;
    if (*connection == NULL) return VPS_CLIENT_OK;
    owned = *connection;
    if (owned->client == NULL || owned->statement_count != 0U) {
        return VPS_CLIENT_INVALID_STATE;
    }
    client = owned->client;
    client->operations.connection_destroy(client->backend_context,
                                          owned->backend_connection);
    owned->backend_connection = NULL;
    if (client->connection_count != 0U) client->connection_count -= 1U;
    vps_client_log(client, VPS_LOG_LEVEL_DEBUG,
                   VPS_CLIENT_OPERATION_CONNECT, "close", "ok");
    vps_memory_release(&client->allocator, (void **)connection,
                       sizeof(*owned));
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_client_statement_open(
    VpsClientConnection *connection,
    const VpsClientStatementSpec *spec,
    VpsClientStatement **statement,
    VpsError *error)
{
    VpsClientStatement *replacement = NULL;
    VpsClient *client;
    VpsClientStatus result;
    if (connection == NULL || connection->client == NULL ||
        !vps_client_statement_spec_valid(spec) || statement == NULL ||
        *statement != NULL) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_PREPARE,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    if ((connection->state != VPS_CLIENT_CONNECTION_READY &&
         connection->state != VPS_CLIENT_CONNECTION_TRANSACTION_ACTIVE) ||
        connection->active_operation != VPS_CLIENT_OPERATION_NONE ||
        connection->statement_count != 0U) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_PREPARE,
                               VPS_CLIENT_INVALID_STATE);
    }
    client = connection->client;
    if (vps_memory_allocate(&client->allocator, sizeof(*replacement),
                            (void **)&replacement) != VPS_MEMORY_OK) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_PREPARE,
                               VPS_CLIENT_OUT_OF_MEMORY);
    }
    (void)memset(replacement, 0, sizeof(*replacement));
    replacement->connection = connection;
    replacement->state = VPS_CLIENT_STATEMENT_NEW;
    result = client->operations.statement_create(
        client->backend_context, connection->backend_connection,
        spec, &replacement->backend_statement, error);
    if (!vps_client_status_valid(result)) result = VPS_CLIENT_BACKEND_ERROR;
    if (result != VPS_CLIENT_OK || replacement->backend_statement == NULL) {
        if (replacement->backend_statement != NULL) {
            client->operations.statement_destroy(
                client->backend_context, replacement->backend_statement);
            replacement->backend_statement = NULL;
        }
        vps_memory_release(&client->allocator, (void **)&replacement,
                           sizeof(*replacement));
        result = result == VPS_CLIENT_OK ? VPS_CLIENT_BACKEND_ERROR : result;
        vps_client_log(client, VPS_LOG_LEVEL_ERROR,
                       VPS_CLIENT_OPERATION_PREPARE, "statement_open",
                       vps_client_status_name(result));
        return result;
    }
    connection->statement_count += 1U;
    *statement = replacement;
    vps_client_log(client, VPS_LOG_LEVEL_DEBUG,
                   VPS_CLIENT_OPERATION_PREPARE, "statement_open", "ok");
    return VPS_CLIENT_OK;
}

static int vps_client_statement_start_allowed(
    const VpsClientStatement *statement,
    VpsClientOperation operation)
{
    if (operation == VPS_CLIENT_OPERATION_CANCEL) {
        return statement->state == VPS_CLIENT_STATEMENT_PREPARING ||
               statement->state == VPS_CLIENT_STATEMENT_EXECUTING ||
               statement->state == VPS_CLIENT_STATEMENT_FETCHING ||
               statement->state == VPS_CLIENT_STATEMENT_ROW_READY;
    }
    if (statement->active_operation != VPS_CLIENT_OPERATION_NONE) return 0;
    if (operation == VPS_CLIENT_OPERATION_PREPARE) {
        return statement->state == VPS_CLIENT_STATEMENT_NEW;
    }
    if (operation == VPS_CLIENT_OPERATION_EXECUTE) {
        return statement->state == VPS_CLIENT_STATEMENT_NEW ||
               statement->state == VPS_CLIENT_STATEMENT_PREPARED;
    }
    if (operation == VPS_CLIENT_OPERATION_FETCH) {
        return statement->state == VPS_CLIENT_STATEMENT_FETCHING;
    }
    return 0;
}

static VpsClientStatementState vps_client_statement_running_state(
    VpsClientOperation operation)
{
    switch (operation) {
    case VPS_CLIENT_OPERATION_PREPARE:
        return VPS_CLIENT_STATEMENT_PREPARING;
    case VPS_CLIENT_OPERATION_EXECUTE:
        return VPS_CLIENT_STATEMENT_EXECUTING;
    case VPS_CLIENT_OPERATION_FETCH:
        return VPS_CLIENT_STATEMENT_FETCHING;
    case VPS_CLIENT_OPERATION_CANCEL:
        return VPS_CLIENT_STATEMENT_CANCELLING;
    default:
        return VPS_CLIENT_STATEMENT_FAILED;
    }
}

VpsClientStatus vps_client_statement_start(
    VpsClientStatement *statement,
    VpsClientOperation operation,
    VpsError *error)
{
    VpsClient *client;
    VpsClientStatus result;
    if (statement == NULL || statement->connection == NULL ||
        !vps_client_operation_valid(operation)) {
        return vps_client_fail(error, operation, VPS_CLIENT_INVALID_ARGUMENT);
    }
    client = statement->connection->client;
    if ((client->operations.capabilities &
         vps_client_operation_capability(operation)) == 0U) {
        return vps_client_fail(error, operation, VPS_CLIENT_UNSUPPORTED);
    }
    if (!vps_client_statement_start_allowed(statement, operation)) {
        return vps_client_fail(error, operation, VPS_CLIENT_INVALID_STATE);
    }
    result = client->operations.statement_start(
        client->backend_context, statement->backend_statement, operation,
        error);
    if (!vps_client_status_valid(result)) result = VPS_CLIENT_BACKEND_ERROR;
    if (result != VPS_CLIENT_OK) {
        statement->state = VPS_CLIENT_STATEMENT_FAILED;
        statement->active_operation = VPS_CLIENT_OPERATION_NONE;
        vps_client_log(client, VPS_LOG_LEVEL_ERROR, operation, "start",
                       vps_client_status_name(result));
        return result;
    }
    statement->active_operation = operation;
    statement->state = vps_client_statement_running_state(operation);
    vps_client_log(client, VPS_LOG_LEVEL_DEBUG, operation,
                   vps_client_statement_state_name(statement->state),
                   "started");
    return VPS_CLIENT_OK;
}

static VpsClientStatus vps_client_statement_publish_row(
    VpsClientStatement *statement,
    VpsError *error)
{
    size_t column_count = 0U;
    VpsClientStatus backend_result;
    backend_result = statement->connection->client->operations.statement_row(
        statement->connection->client->backend_context,
        statement->backend_statement, &column_count, error);
    if (!vps_client_status_valid(backend_result)) {
        backend_result = VPS_CLIENT_BACKEND_ERROR;
    }
    if (backend_result != VPS_CLIENT_OK ||
        column_count > VPS_CLIENT_MAX_RESULT_FIELD_COUNT) {
        statement->state = VPS_CLIENT_STATEMENT_FAILED;
        statement->active_operation = VPS_CLIENT_OPERATION_NONE;
        return backend_result == VPS_CLIENT_OK ? VPS_CLIENT_BACKEND_ERROR
                                                : backend_result;
    }
    if (statement->row_generation == UINT64_MAX) {
        statement->state = VPS_CLIENT_STATEMENT_FAILED;
        statement->active_operation = VPS_CLIENT_OPERATION_NONE;
        return vps_client_fail(error, VPS_CLIENT_OPERATION_FETCH,
                               VPS_CLIENT_LIMIT_EXCEEDED);
    }
    statement->row_generation += 1U;
    statement->active_operation = VPS_CLIENT_OPERATION_NONE;
    statement->state = VPS_CLIENT_STATEMENT_ROW_READY;
    statement->row_view.statement = statement;
    statement->row_view.generation = statement->row_generation;
    statement->row_view.column_count = column_count;
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_client_statement_poll(
    VpsClientStatement *statement,
    VpsClientPollResult *poll_result,
    VpsError *error)
{
    VpsClient *client;
    VpsClientPollResult observed;
    VpsClientOperation operation;
    VpsClientStatus result;
    if (statement == NULL || statement->connection == NULL ||
        poll_result == NULL) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_NONE,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    (void)memset(poll_result, 0, sizeof(*poll_result));
    operation = statement->active_operation;
    if (operation != VPS_CLIENT_OPERATION_PREPARE &&
        operation != VPS_CLIENT_OPERATION_EXECUTE &&
        operation != VPS_CLIENT_OPERATION_FETCH &&
        operation != VPS_CLIENT_OPERATION_CANCEL) {
        return vps_client_fail(error, operation, VPS_CLIENT_INVALID_STATE);
    }
    client = statement->connection->client;
    (void)memset(&observed, 0, sizeof(observed));
    observed.outcome = (VpsClientPollOutcome)-1;
    result = client->operations.statement_poll(
        client->backend_context, statement->backend_statement, &observed,
        error);
    if (!vps_client_status_valid(result)) result = VPS_CLIENT_BACKEND_ERROR;
    if (result != VPS_CLIENT_OK ||
        !vps_client_poll_outcome_valid(observed.outcome) ||
        (observed.outcome == VPS_CLIENT_POLL_WAIT &&
         !vps_client_wait_valid(&observed.wait, operation)) ||
        (observed.outcome == VPS_CLIENT_POLL_ROW_READY &&
         operation != VPS_CLIENT_OPERATION_FETCH)) {
        result = result == VPS_CLIENT_OK ? VPS_CLIENT_BACKEND_ERROR : result;
        statement->state = VPS_CLIENT_STATEMENT_FAILED;
        statement->active_operation = VPS_CLIENT_OPERATION_NONE;
        vps_client_log(client, VPS_LOG_LEVEL_ERROR, operation, "poll",
                       vps_client_status_name(result));
        return result;
    }
    *poll_result = observed;
    if (observed.outcome == VPS_CLIENT_POLL_WAIT) {
        vps_client_log(client, VPS_LOG_LEVEL_DEBUG, operation, "poll",
                       "wait");
        return VPS_CLIENT_OK;
    }
    if (observed.outcome == VPS_CLIENT_POLL_FAILED) {
        statement->state = VPS_CLIENT_STATEMENT_FAILED;
        statement->active_operation = VPS_CLIENT_OPERATION_NONE;
        vps_client_log(client, VPS_LOG_LEVEL_ERROR, operation, "poll",
                       "failed");
        return VPS_CLIENT_BACKEND_ERROR;
    }
    if (observed.outcome == VPS_CLIENT_POLL_ROW_READY) {
        result = vps_client_statement_publish_row(statement, error);
        vps_client_log(client,
                       result == VPS_CLIENT_OK ? VPS_LOG_LEVEL_DEBUG
                                               : VPS_LOG_LEVEL_ERROR,
                       operation,
                       vps_client_statement_state_name(statement->state),
                       vps_client_status_name(result));
        return result;
    }
    statement->active_operation = VPS_CLIENT_OPERATION_NONE;
    switch (operation) {
    case VPS_CLIENT_OPERATION_PREPARE:
        statement->state = VPS_CLIENT_STATEMENT_PREPARED;
        break;
    case VPS_CLIENT_OPERATION_EXECUTE:
        statement->state = VPS_CLIENT_STATEMENT_FETCHING;
        break;
    case VPS_CLIENT_OPERATION_FETCH:
    case VPS_CLIENT_OPERATION_CANCEL:
        statement->state = VPS_CLIENT_STATEMENT_COMPLETE;
        break;
    default:
        statement->state = VPS_CLIENT_STATEMENT_FAILED;
        return vps_client_fail(error, operation, VPS_CLIENT_INVALID_STATE);
    }
    vps_client_log(client, VPS_LOG_LEVEL_DEBUG, operation,
                   vps_client_statement_state_name(statement->state),
                   "complete");
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_client_statement_wait(
    VpsClientStatement *statement,
    const VpsClientWaitRequest *wait_request,
    VpsError *error)
{
    VpsClientOperation operation;
    VpsClientStatus result;
    if (statement == NULL || statement->connection == NULL ||
        wait_request == NULL) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_NONE,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    operation = statement->active_operation;
    if ((operation != VPS_CLIENT_OPERATION_PREPARE &&
         operation != VPS_CLIENT_OPERATION_EXECUTE &&
         operation != VPS_CLIENT_OPERATION_FETCH &&
         operation != VPS_CLIENT_OPERATION_CANCEL) ||
        !vps_client_wait_valid(wait_request, operation)) {
        return vps_client_fail(error, operation, VPS_CLIENT_INVALID_STATE);
    }
    result = statement->connection->client->operations.statement_wait(
        statement->connection->client->backend_context,
        statement->backend_statement, wait_request, error);
    if (!vps_client_status_valid(result)) result = VPS_CLIENT_BACKEND_ERROR;
    if (result != VPS_CLIENT_OK) {
        statement->state = VPS_CLIENT_STATEMENT_FAILED;
        statement->active_operation = VPS_CLIENT_OPERATION_NONE;
        vps_client_log(statement->connection->client, VPS_LOG_LEVEL_ERROR,
                       operation, "wait", vps_client_status_name(result));
        return result;
    }
    vps_client_log(statement->connection->client, VPS_LOG_LEVEL_DEBUG,
                   operation, "wait", "ready");
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_client_statement_metadata(
    const VpsClientStatement *statement,
    VpsClientStatementMetadata *metadata,
    VpsError *error)
{
    VpsClientStatus result;
    if (statement == NULL || statement->connection == NULL ||
        metadata == NULL) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_PREPARE,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    if (statement->state == VPS_CLIENT_STATEMENT_NEW ||
        statement->state == VPS_CLIENT_STATEMENT_PREPARING ||
        statement->state == VPS_CLIENT_STATEMENT_FAILED ||
        statement->state == VPS_CLIENT_STATEMENT_CANCELLING) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_PREPARE,
                               VPS_CLIENT_INVALID_STATE);
    }
    (void)memset(metadata, 0, sizeof(*metadata));
    result = statement->connection->client->operations.statement_metadata(
        statement->connection->client->backend_context,
        statement->backend_statement, metadata, error);
    if (!vps_client_status_valid(result)) result = VPS_CLIENT_BACKEND_ERROR;
    return result;
}

VpsClientStatus vps_client_statement_result_field(
    const VpsClientStatement *statement,
    size_t field_index,
    VpsClientResultFieldMetadata *field,
    VpsError *error)
{
    VpsClientStatementMetadata metadata;
    VpsClientStatus result;
    if (statement == NULL || field == NULL) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_PREPARE,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    result = vps_client_statement_metadata(statement, &metadata, error);
    if (result != VPS_CLIENT_OK) return result;
    if (field_index >= metadata.result_field_count) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_PREPARE,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    (void)memset(field, 0, sizeof(*field));
    result = statement->connection->client->operations.statement_result_field(
        statement->connection->client->backend_context,
        statement->backend_statement, field_index, field, error);
    if (!vps_client_status_valid(result)) return VPS_CLIENT_BACKEND_ERROR;
    return result;
}

VpsClientStatus vps_client_statement_current_row(
    VpsClientStatement *statement,
    const VpsClientRowView **row,
    VpsError *error)
{
    if (statement == NULL || row == NULL || *row != NULL) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_FETCH,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    if (statement->state != VPS_CLIENT_STATEMENT_ROW_READY ||
        statement->row_view.statement != statement ||
        statement->row_view.generation != statement->row_generation) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_FETCH,
                               VPS_CLIENT_INVALID_STATE);
    }
    *row = &statement->row_view;
    return VPS_CLIENT_OK;
}

size_t vps_client_row_column_count(const VpsClientRowView *row)
{
    return row == NULL ? 0U : row->column_count;
}

VpsClientStatus vps_client_row_column(
    const VpsClientRowView *row,
    size_t column_index,
    VpsClientColumnView *column,
    VpsError *error)
{
    VpsClientStatus result;
    if (row == NULL || row->statement == NULL || column == NULL ||
        column_index >= row->column_count) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_FETCH,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    if (!vps_client_statement_row_is_current(row->statement,
                                             row->generation)) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_FETCH,
                               VPS_CLIENT_INVALID_STATE);
    }
    (void)memset(column, 0, sizeof(*column));
    result = row->statement->connection->client->operations.statement_column(
        row->statement->connection->client->backend_context,
        row->statement->backend_statement, column_index, column, error);
    if (!vps_client_status_valid(result)) result = VPS_CLIENT_BACKEND_ERROR;
    return result;
}

VpsClientStatus vps_client_statement_row_consumed(
    VpsClientStatement *statement,
    VpsError *error)
{
    if (statement == NULL || statement->connection == NULL) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_FETCH,
                               VPS_CLIENT_INVALID_ARGUMENT);
    }
    if (statement->state != VPS_CLIENT_STATEMENT_ROW_READY ||
        statement->active_operation != VPS_CLIENT_OPERATION_NONE) {
        return vps_client_fail(error, VPS_CLIENT_OPERATION_FETCH,
                               VPS_CLIENT_INVALID_STATE);
    }
    if (statement->row_generation == UINT64_MAX) {
        statement->state = VPS_CLIENT_STATEMENT_FAILED;
        return vps_client_fail(error, VPS_CLIENT_OPERATION_FETCH,
                               VPS_CLIENT_LIMIT_EXCEEDED);
    }
    statement->connection->client->operations.statement_row_release(
        statement->connection->client->backend_context,
        statement->backend_statement);
    statement->row_generation += 1U;
    statement->state = VPS_CLIENT_STATEMENT_FETCHING;
    (void)memset(&statement->row_view, 0, sizeof(statement->row_view));
    vps_client_log(statement->connection->client, VPS_LOG_LEVEL_DEBUG,
                   VPS_CLIENT_OPERATION_FETCH, "row", "consumed");
    return VPS_CLIENT_OK;
}

VpsClientStatus vps_client_statement_close(
    VpsClientStatement **statement)
{
    VpsClientStatement *owned;
    VpsClientConnection *connection;
    VpsClient *client;
    if (statement == NULL) return VPS_CLIENT_INVALID_ARGUMENT;
    if (*statement == NULL) return VPS_CLIENT_OK;
    owned = *statement;
    if (owned->connection == NULL || owned->connection->client == NULL) {
        return VPS_CLIENT_INVALID_STATE;
    }
    connection = owned->connection;
    client = connection->client;
    if (owned->state == VPS_CLIENT_STATEMENT_ROW_READY) {
        client->operations.statement_row_release(
            client->backend_context, owned->backend_statement);
    }
    client->operations.statement_destroy(client->backend_context,
                                         owned->backend_statement);
    owned->backend_statement = NULL;
    if (connection->statement_count != 0U) connection->statement_count -= 1U;
    vps_client_log(client, VPS_LOG_LEVEL_DEBUG,
                   VPS_CLIENT_OPERATION_EXECUTE, "statement_close", "ok");
    vps_memory_release(&client->allocator, (void **)statement,
                       sizeof(*owned));
    return VPS_CLIENT_OK;
}

VpsClientConnectionState vps_client_connection_state(
    const VpsClientConnection *connection)
{
    return connection == NULL ? VPS_CLIENT_CONNECTION_FAILED
                              : connection->state;
}

VpsClientStatementState vps_client_statement_state(
    const VpsClientStatement *statement)
{
    return statement == NULL ? VPS_CLIENT_STATEMENT_FAILED
                             : statement->state;
}

uint64_t vps_client_statement_row_generation(
    const VpsClientStatement *statement)
{
    return statement == NULL ? UINT64_C(0) : statement->row_generation;
}

int vps_client_statement_row_is_current(
    const VpsClientStatement *statement,
    uint64_t generation)
{
    return statement != NULL &&
           statement->state == VPS_CLIENT_STATEMENT_ROW_READY &&
           generation != UINT64_C(0) &&
           statement->row_generation == generation;
}

const char *vps_client_status_name(VpsClientStatus status)
{
    switch (status) {
    case VPS_CLIENT_OK: return "ok";
    case VPS_CLIENT_INVALID_ARGUMENT: return "invalid_argument";
    case VPS_CLIENT_INVALID_STATE: return "invalid_state";
    case VPS_CLIENT_UNSUPPORTED: return "unsupported";
    case VPS_CLIENT_OUT_OF_MEMORY: return "out_of_memory";
    case VPS_CLIENT_LIMIT_EXCEEDED: return "limit_exceeded";
    case VPS_CLIENT_BACKEND_ERROR: return "backend_error";
    default: return "unknown";
    }
}

const char *vps_client_operation_name(VpsClientOperation operation)
{
    switch (operation) {
    case VPS_CLIENT_OPERATION_NONE: return "none";
    case VPS_CLIENT_OPERATION_CONNECT: return "connect";
    case VPS_CLIENT_OPERATION_PREPARE: return "prepare";
    case VPS_CLIENT_OPERATION_EXECUTE: return "execute";
    case VPS_CLIENT_OPERATION_FETCH: return "fetch";
    case VPS_CLIENT_OPERATION_BEGIN: return "begin";
    case VPS_CLIENT_OPERATION_COMMIT: return "commit";
    case VPS_CLIENT_OPERATION_ROLLBACK: return "rollback";
    case VPS_CLIENT_OPERATION_RESET: return "reset";
    case VPS_CLIENT_OPERATION_PING: return "ping";
    case VPS_CLIENT_OPERATION_CANCEL: return "cancel";
    default: return "unknown";
    }
}

const char *vps_client_connection_state_name(VpsClientConnectionState state)
{
    switch (state) {
    case VPS_CLIENT_CONNECTION_NEW: return "new";
    case VPS_CLIENT_CONNECTION_CONNECTING: return "connecting";
    case VPS_CLIENT_CONNECTION_READY: return "ready";
    case VPS_CLIENT_CONNECTION_TRANSACTION_STARTING:
        return "transaction_starting";
    case VPS_CLIENT_CONNECTION_TRANSACTION_ACTIVE:
        return "transaction_active";
    case VPS_CLIENT_CONNECTION_TRANSACTION_ENDING:
        return "transaction_ending";
    case VPS_CLIENT_CONNECTION_RESETTING: return "resetting";
    case VPS_CLIENT_CONNECTION_PINGING: return "pinging";
    case VPS_CLIENT_CONNECTION_CANCELLING: return "cancelling";
    case VPS_CLIENT_CONNECTION_FAILED: return "failed";
    default: return "unknown";
    }
}

const char *vps_client_statement_state_name(VpsClientStatementState state)
{
    switch (state) {
    case VPS_CLIENT_STATEMENT_NEW: return "new";
    case VPS_CLIENT_STATEMENT_PREPARING: return "preparing";
    case VPS_CLIENT_STATEMENT_PREPARED: return "prepared";
    case VPS_CLIENT_STATEMENT_EXECUTING: return "executing";
    case VPS_CLIENT_STATEMENT_FETCHING: return "fetching";
    case VPS_CLIENT_STATEMENT_ROW_READY: return "row_ready";
    case VPS_CLIENT_STATEMENT_COMPLETE: return "complete";
    case VPS_CLIENT_STATEMENT_CANCELLING: return "cancelling";
    case VPS_CLIENT_STATEMENT_FAILED: return "failed";
    default: return "unknown";
    }
}

const char *vps_client_poll_outcome_name(VpsClientPollOutcome outcome)
{
    switch (outcome) {
    case VPS_CLIENT_POLL_WAIT: return "wait";
    case VPS_CLIENT_POLL_COMPLETE: return "complete";
    case VPS_CLIENT_POLL_ROW_READY: return "row_ready";
    case VPS_CLIENT_POLL_FAILED: return "failed";
    default: return "unknown";
    }
}

const char *vps_client_wait_phase_name(VpsClientWaitPhase phase)
{
    switch (phase) {
    case VPS_CLIENT_WAIT_CONNECT: return "connect";
    case VPS_CLIENT_WAIT_PREPARE: return "prepare";
    case VPS_CLIENT_WAIT_EXECUTE: return "execute";
    case VPS_CLIENT_WAIT_FETCH: return "fetch";
    case VPS_CLIENT_WAIT_TRANSACTION: return "transaction";
    case VPS_CLIENT_WAIT_RESET: return "reset";
    case VPS_CLIENT_WAIT_PING: return "ping";
    case VPS_CLIENT_WAIT_CANCEL: return "cancel";
    default: return "unknown";
    }
}
