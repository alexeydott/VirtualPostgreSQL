#include "vps_client.h"

#include <stdio.h>
#include <string.h>

#define TEST_CHECK(condition, name)                                 \
    do {                                                            \
        if (!(condition)) {                                         \
            (void)fprintf(stderr, "client_case=%s status=failed\n", \
                          (name));                                  \
            return 0;                                               \
        }                                                           \
    } while (0)

typedef struct FakeBackend {
    int connection_token;
    int statement_token;
    size_t connection_create_count;
    size_t connection_destroy_count;
    size_t connection_start_count;
    size_t connection_poll_count;
    size_t connection_wait_count;
    size_t statement_create_count;
    size_t statement_destroy_count;
    size_t statement_start_count;
    size_t statement_poll_count;
    VpsClientStatus connection_create_status;
    VpsClientStatus connection_start_status;
    VpsClientStatus connection_poll_status;
    VpsClientStatus connection_wait_status;
    VpsClientStatus statement_create_status;
    VpsClientStatus statement_start_status;
    VpsClientStatus statement_poll_status;
    VpsClientPollResult connection_poll_result;
    VpsClientPollResult statement_poll_result;
    int partial_connection;
    int partial_statement;
    VpsErrorClass poll_error_class;
} FakeBackend;

static const char test_query[] = "SELECT 1";
static const VpsClientStatementSpec test_statement_spec = {
    test_query, sizeof(test_query) - 1U, NULL, 0U, NULL, 0U,
    UINT64_C(1000), 0, 0};

typedef struct TestLogCapture {
    size_t event_count;
    size_t operation_count;
    size_t phase_count;
    size_t status_count;
    int forbidden_seen;
} TestLogCapture;

static int test_log_sink(void *context, const VpsLogEvent *event)
{
    static const char forbidden[] = "synthetic-secret";
    TestLogCapture *capture = (TestLogCapture *)context;
    size_t index;
    capture->event_count += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->key == VPS_LOG_FIELD_OPERATION) {
            capture->operation_count += 1U;
        } else if (field->key == VPS_LOG_FIELD_PHASE) {
            capture->phase_count += 1U;
        } else if (field->key == VPS_LOG_FIELD_STATUS) {
            capture->status_count += 1U;
        }
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            field->value.string_value.length == sizeof(forbidden) - 1U &&
            memcmp(field->value.string_value.data, forbidden,
                   sizeof(forbidden) - 1U) == 0) {
            capture->forbidden_seen = 1;
        }
    }
    return 0;
}

static void fake_init(FakeBackend *backend)
{
    (void)memset(backend, 0, sizeof(*backend));
    backend->connection_create_status = VPS_CLIENT_OK;
    backend->connection_start_status = VPS_CLIENT_OK;
    backend->connection_poll_status = VPS_CLIENT_OK;
    backend->connection_wait_status = VPS_CLIENT_OK;
    backend->statement_create_status = VPS_CLIENT_OK;
    backend->statement_start_status = VPS_CLIENT_OK;
    backend->statement_poll_status = VPS_CLIENT_OK;
    backend->connection_poll_result.outcome = VPS_CLIENT_POLL_COMPLETE;
    backend->statement_poll_result.outcome = VPS_CLIENT_POLL_COMPLETE;
}

static VpsClientStatus fake_connection_create(void *context,
                                               void **handle,
                                               VpsError *error)
{
    FakeBackend *backend = (FakeBackend *)context;
    (void)error;
    backend->connection_create_count += 1U;
    if (backend->connection_create_status == VPS_CLIENT_OK ||
        backend->partial_connection) {
        *handle = &backend->connection_token;
    }
    return backend->connection_create_status;
}

static VpsClientStatus fake_connection_start(void *context,
                                              void *handle,
                                              VpsClientOperation operation,
                                              VpsError *error)
{
    FakeBackend *backend = (FakeBackend *)context;
    (void)operation;
    (void)error;
    if (handle != &backend->connection_token) return VPS_CLIENT_BACKEND_ERROR;
    backend->connection_start_count += 1U;
    return backend->connection_start_status;
}

static VpsClientStatus fake_connection_poll(void *context,
                                             void *handle,
                                             VpsClientPollResult *result,
                                             VpsError *error)
{
    FakeBackend *backend = (FakeBackend *)context;
    if (handle != &backend->connection_token) return VPS_CLIENT_BACKEND_ERROR;
    backend->connection_poll_count += 1U;
    *result = backend->connection_poll_result;
    if (result->outcome == VPS_CLIENT_POLL_FAILED && error != NULL &&
        error->initialized && backend->poll_error_class != VPS_ERROR_CLASS_NONE) {
        (void)vps_error_set_local(error, VPS_ERROR_OPERATION_QUERY,
                                  backend->poll_error_class, NULL);
    }
    return backend->connection_poll_status;
}

static void fake_connection_destroy(void *context, void *handle)
{
    FakeBackend *backend = (FakeBackend *)context;
    if (handle == &backend->connection_token) {
        backend->connection_destroy_count += 1U;
    }
}

static VpsClientStatus fake_connection_wait(
    void *context,
    void *handle,
    const VpsClientWaitRequest *request,
    VpsError *error)
{
    FakeBackend *backend = (FakeBackend *)context;
    (void)error;
    if (handle != &backend->connection_token || request == NULL) {
        return VPS_CLIENT_BACKEND_ERROR;
    }
    backend->connection_wait_count += 1U;
    return backend->connection_wait_status;
}

static VpsClientStatus fake_statement_create(void *context,
                                              void *connection,
                                              const VpsClientStatementSpec *spec,
                                              void **statement,
                                              VpsError *error)
{
    FakeBackend *backend = (FakeBackend *)context;
    (void)error;
    if (connection != &backend->connection_token || spec == NULL) {
        return VPS_CLIENT_BACKEND_ERROR;
    }
    backend->statement_create_count += 1U;
    if (backend->statement_create_status == VPS_CLIENT_OK ||
        backend->partial_statement) {
        *statement = &backend->statement_token;
    }
    return backend->statement_create_status;
}

static VpsClientStatus fake_statement_start(void *context,
                                             void *handle,
                                             VpsClientOperation operation,
                                             VpsError *error)
{
    FakeBackend *backend = (FakeBackend *)context;
    (void)operation;
    (void)error;
    if (handle != &backend->statement_token) return VPS_CLIENT_BACKEND_ERROR;
    backend->statement_start_count += 1U;
    return backend->statement_start_status;
}

static VpsClientStatus fake_statement_poll(void *context,
                                            void *handle,
                                            VpsClientPollResult *result,
                                            VpsError *error)
{
    FakeBackend *backend = (FakeBackend *)context;
    if (handle != &backend->statement_token) return VPS_CLIENT_BACKEND_ERROR;
    backend->statement_poll_count += 1U;
    *result = backend->statement_poll_result;
    if (result->outcome == VPS_CLIENT_POLL_FAILED && error != NULL &&
        error->initialized && backend->poll_error_class != VPS_ERROR_CLASS_NONE) {
        (void)vps_error_set_local(error, VPS_ERROR_OPERATION_QUERY,
                                  backend->poll_error_class, NULL);
    }
    return backend->statement_poll_status;
}

static VpsClientStatus fake_statement_wait(
    void *context, void *handle, const VpsClientWaitRequest *request,
    VpsError *error)
{
    FakeBackend *backend = (FakeBackend *)context;
    (void)error;
    if (handle != &backend->statement_token || request == NULL) {
        return VPS_CLIENT_BACKEND_ERROR;
    }
    return VPS_CLIENT_OK;
}

static VpsClientStatus fake_statement_metadata(
    void *context, const void *handle, VpsClientStatementMetadata *metadata,
    VpsError *error)
{
    FakeBackend *backend = (FakeBackend *)context;
    (void)error;
    if (handle != &backend->statement_token || metadata == NULL) {
        return VPS_CLIENT_BACKEND_ERROR;
    }
    (void)memset(metadata, 0, sizeof(*metadata));
    metadata->described = 1;
    return VPS_CLIENT_OK;
}

static VpsClientStatus fake_statement_row(
    void *context, const void *handle, size_t *column_count, VpsError *error)
{
    FakeBackend *backend = (FakeBackend *)context;
    (void)error;
    if (handle != &backend->statement_token || column_count == NULL) {
        return VPS_CLIENT_BACKEND_ERROR;
    }
    *column_count = 1U;
    return VPS_CLIENT_OK;
}

static VpsClientStatus fake_statement_column(
    void *context, const void *handle, size_t index,
    VpsClientColumnView *column, VpsError *error)
{
    static const char value[] = "1";
    FakeBackend *backend = (FakeBackend *)context;
    (void)error;
    if (handle != &backend->statement_token || index != 0U ||
        column == NULL) return VPS_CLIENT_BACKEND_ERROR;
    column->data = value;
    column->length = sizeof(value) - 1U;
    column->type_oid = 23U;
    column->format = VPS_CLIENT_VALUE_TEXT;
    return VPS_CLIENT_OK;
}

static void fake_statement_row_release(void *context, void *handle)
{ (void)context; (void)handle; }

static void fake_statement_destroy(void *context, void *handle)
{
    FakeBackend *backend = (FakeBackend *)context;
    if (handle == &backend->statement_token) {
        backend->statement_destroy_count += 1U;
    }
}

static VpsClientOperations fake_operations(uint64_t capabilities)
{
    VpsClientOperations operations;
    (void)memset(&operations, 0, sizeof(operations));
    operations.structure_size = sizeof(operations);
    operations.contract_version = VPS_CLIENT_CONTRACT_VERSION;
    operations.capabilities = capabilities;
    operations.connection_create = fake_connection_create;
    operations.connection_start = fake_connection_start;
    operations.connection_poll = fake_connection_poll;
    operations.connection_wait = fake_connection_wait;
    operations.connection_destroy = fake_connection_destroy;
    operations.statement_create = fake_statement_create;
    operations.statement_start = fake_statement_start;
    operations.statement_poll = fake_statement_poll;
    operations.statement_wait = fake_statement_wait;
    operations.statement_metadata = fake_statement_metadata;
    operations.statement_row = fake_statement_row;
    operations.statement_column = fake_statement_column;
    operations.statement_row_release = fake_statement_row_release;
    operations.statement_destroy = fake_statement_destroy;
    return operations;
}

static int test_connect_ready(VpsClientConnection *connection,
                              FakeBackend *backend)
{
    VpsClientPollResult result;
    backend->connection_poll_result.outcome = VPS_CLIENT_POLL_COMPLETE;
    return vps_client_connection_start(connection,
                                       VPS_CLIENT_OPERATION_CONNECT, NULL) ==
               VPS_CLIENT_OK &&
           vps_client_connection_poll(connection, &result, NULL) ==
               VPS_CLIENT_OK &&
           result.outcome == VPS_CLIENT_POLL_COMPLETE &&
           vps_client_connection_state(connection) ==
               VPS_CLIENT_CONNECTION_READY;
}

static int test_abi_and_lifecycle(void)
{
    VpsAllocator allocator;
    VpsClient client;
    VpsClientOperations operations;
    FakeBackend backend;
    VpsClientConnection *connection = NULL;
    struct ExtendedOperations {
        VpsClientOperations base;
        uint64_t reserved;
    } extended;
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK, "abi_allocator");
    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    operations.contract_version += 1U;
    TEST_CHECK(vps_client_init(&client, &allocator, &operations, &backend,
                               NULL) == VPS_CLIENT_INVALID_ARGUMENT,
               "abi_version");
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    operations.structure_size -= 1U;
    TEST_CHECK(vps_client_init(&client, &allocator, &operations, &backend,
                               NULL) == VPS_CLIENT_INVALID_ARGUMENT,
               "abi_size");
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    operations.statement_poll = NULL;
    TEST_CHECK(vps_client_init(&client, &allocator, &operations, &backend,
                               NULL) == VPS_CLIENT_INVALID_ARGUMENT,
               "abi_callback");
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    operations.connection_wait = NULL;
    TEST_CHECK(vps_client_init(&client, &allocator, &operations, &backend,
                               NULL) == VPS_CLIENT_INVALID_ARGUMENT,
               "abi_wait_callback");
    operations = fake_operations(VPS_CLIENT_CAP_ALL | (UINT64_C(1) << 40));
    TEST_CHECK(vps_client_init(&client, &allocator, &operations, &backend,
                               NULL) == VPS_CLIENT_INVALID_ARGUMENT,
               "abi_capability");
    (void)memset(&extended, 0, sizeof(extended));
    extended.base = fake_operations(VPS_CLIENT_CAP_ALL);
    extended.base.structure_size = sizeof(extended);
    TEST_CHECK(vps_client_init(&client, &allocator, &extended.base, &backend,
                               NULL) == VPS_CLIENT_OK,
               "abi_extended");
    TEST_CHECK(vps_client_connection_open(&client, &connection, NULL) ==
                   VPS_CLIENT_OK,
               "abi_open");
    TEST_CHECK(vps_client_cleanup(&client) == VPS_CLIENT_INVALID_STATE,
               "abi_live_child");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   connection == NULL &&
                   backend.connection_destroy_count == 1U,
               "abi_close");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   backend.connection_destroy_count == 1U,
               "abi_close_repeat");
    TEST_CHECK(vps_client_cleanup(&client) == VPS_CLIENT_OK &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "abi_cleanup_repeat");
    return 1;
}

static int test_connection_machine(void)
{
    VpsAllocator allocator;
    VpsClient client;
    VpsClientOperations operations;
    FakeBackend backend;
    VpsClientConnection *connection = NULL;
    VpsClientPollResult result;
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK, "conn_allocator");
    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    TEST_CHECK(vps_client_init(&client, &allocator, &operations, &backend,
                               NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, NULL) ==
                       VPS_CLIENT_OK,
               "conn_setup");
    TEST_CHECK(vps_client_connection_start(connection,
                                           VPS_CLIENT_OPERATION_PING, NULL) ==
                   VPS_CLIENT_INVALID_STATE,
               "conn_invalid_transition");
    TEST_CHECK(vps_client_connection_start(connection,
                                           VPS_CLIENT_OPERATION_CONNECT,
                                           NULL) == VPS_CLIENT_OK,
               "conn_connect_start");
    backend.connection_poll_result.outcome = VPS_CLIENT_POLL_WAIT;
    backend.connection_poll_result.wait.interest = VPS_CLIENT_WAIT_WRITE;
    backend.connection_poll_result.wait.phase = VPS_CLIENT_WAIT_CONNECT;
    backend.connection_poll_result.wait.max_slice_ms = 25U;
    TEST_CHECK(vps_client_connection_poll(connection, &result, NULL) ==
                   VPS_CLIENT_OK &&
                   result.outcome == VPS_CLIENT_POLL_WAIT &&
                   result.wait.max_slice_ms == 25U,
               "conn_would_block");
    TEST_CHECK(vps_client_connection_wait(connection, &result.wait, NULL) ==
                   VPS_CLIENT_OK && backend.connection_wait_count == 1U,
               "conn_wait_ready");
    backend.connection_poll_result.outcome = VPS_CLIENT_POLL_COMPLETE;
    TEST_CHECK(vps_client_connection_poll(connection, &result, NULL) ==
                   VPS_CLIENT_OK &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_READY,
               "conn_connect_complete");
    TEST_CHECK(vps_client_connection_start(connection,
                                           VPS_CLIENT_OPERATION_BEGIN,
                                           NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_poll(connection, &result, NULL) ==
                       VPS_CLIENT_OK &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_TRANSACTION_ACTIVE,
               "conn_begin");
    TEST_CHECK(vps_client_connection_start(connection,
                                           VPS_CLIENT_OPERATION_COMMIT,
                                           NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_poll(connection, &result, NULL) ==
                       VPS_CLIENT_OK &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_READY,
               "conn_commit");
    TEST_CHECK(vps_client_connection_start(connection,
                                           VPS_CLIENT_OPERATION_RESET,
                                           NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_poll(connection, &result, NULL) ==
                       VPS_CLIENT_OK &&
                   vps_client_connection_start(connection,
                                               VPS_CLIENT_OPERATION_PING,
                                               NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_poll(connection, &result, NULL) ==
                       VPS_CLIENT_OK,
               "conn_reset_ping");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "conn_cleanup");
    return 1;
}

static int test_statement_rows_and_cancel(void)
{
    VpsAllocator allocator;
    VpsClient client;
    VpsClientOperations operations;
    FakeBackend backend;
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;
    VpsClientPollResult result;
    uint64_t generation;
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK, "stmt_allocator");
    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    TEST_CHECK(vps_client_init(&client, &allocator, &operations, &backend,
                               NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, NULL) ==
                       VPS_CLIENT_OK &&
                   test_connect_ready(connection, &backend) &&
                   vps_client_statement_open(connection, &test_statement_spec,
                                             &statement, NULL) ==
                       VPS_CLIENT_OK,
               "stmt_setup");
    TEST_CHECK(vps_client_connection_close(&connection) ==
                   VPS_CLIENT_INVALID_STATE && connection != NULL,
               "stmt_parent_lifetime");
    TEST_CHECK(vps_client_statement_start(statement,
                                          VPS_CLIENT_OPERATION_PREPARE,
                                          NULL) == VPS_CLIENT_OK &&
                   vps_client_statement_poll(statement, &result, NULL) ==
                       VPS_CLIENT_OK &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_PREPARED,
               "stmt_prepare");
    TEST_CHECK(vps_client_statement_start(statement,
                                          VPS_CLIENT_OPERATION_EXECUTE,
                                          NULL) == VPS_CLIENT_OK,
               "stmt_execute_start");
    backend.statement_poll_result.outcome = VPS_CLIENT_POLL_WAIT;
    backend.statement_poll_result.wait.interest = VPS_CLIENT_WAIT_READ_WRITE;
    backend.statement_poll_result.wait.phase = VPS_CLIENT_WAIT_EXECUTE;
    backend.statement_poll_result.wait.max_slice_ms = 50U;
    TEST_CHECK(vps_client_statement_poll(statement, &result, NULL) ==
                   VPS_CLIENT_OK && result.outcome == VPS_CLIENT_POLL_WAIT,
               "stmt_execute_wait");
    backend.statement_poll_result.outcome = VPS_CLIENT_POLL_COMPLETE;
    TEST_CHECK(vps_client_statement_poll(statement, &result, NULL) ==
                   VPS_CLIENT_OK &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_FETCHING,
               "stmt_execute_complete");
    TEST_CHECK(vps_client_statement_start(statement,
                                          VPS_CLIENT_OPERATION_FETCH,
                                          NULL) == VPS_CLIENT_OK,
               "stmt_fetch_start");
    backend.statement_poll_result.outcome = VPS_CLIENT_POLL_ROW_READY;
    TEST_CHECK(vps_client_statement_poll(statement, &result, NULL) ==
                   VPS_CLIENT_OK &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_ROW_READY,
               "stmt_row_ready");
    generation = vps_client_statement_row_generation(statement);
    TEST_CHECK(generation != 0U &&
                   vps_client_statement_row_is_current(statement, generation),
               "stmt_row_current");
    TEST_CHECK(vps_client_statement_row_consumed(statement, NULL) ==
                   VPS_CLIENT_OK &&
                   !vps_client_statement_row_is_current(statement, generation) &&
                   vps_client_statement_row_consumed(statement, NULL) ==
                       VPS_CLIENT_INVALID_STATE,
               "stmt_row_invalidate");
    TEST_CHECK(vps_client_statement_start(statement,
                                          VPS_CLIENT_OPERATION_FETCH,
                                          NULL) == VPS_CLIENT_OK,
               "stmt_fetch_end_start");
    backend.statement_poll_result.outcome = VPS_CLIENT_POLL_COMPLETE;
    TEST_CHECK(vps_client_statement_poll(statement, &result, NULL) ==
                   VPS_CLIENT_OK &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_COMPLETE,
               "stmt_fetch_end");
    TEST_CHECK(vps_client_statement_close(&statement) == VPS_CLIENT_OK &&
                   vps_client_statement_close(&statement) == VPS_CLIENT_OK &&
                   backend.statement_destroy_count == 1U &&
                   vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "stmt_cleanup");

    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    TEST_CHECK(vps_client_init(&client, &allocator, &operations, &backend,
                               NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, NULL) ==
                       VPS_CLIENT_OK && test_connect_ready(connection, &backend) &&
                   vps_client_statement_open(connection, &test_statement_spec,
                                             &statement, NULL) ==
                       VPS_CLIENT_OK &&
                   vps_client_statement_start(statement,
                                              VPS_CLIENT_OPERATION_EXECUTE,
                                              NULL) == VPS_CLIENT_OK &&
                   vps_client_statement_start(statement,
                                              VPS_CLIENT_OPERATION_CANCEL,
                                              NULL) == VPS_CLIENT_OK,
               "stmt_cancel_start");
    backend.statement_poll_result.outcome = VPS_CLIENT_POLL_COMPLETE;
    TEST_CHECK(vps_client_statement_poll(statement, &result, NULL) ==
                   VPS_CLIENT_OK &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_COMPLETE,
               "stmt_cancel_complete");
    TEST_CHECK(vps_client_statement_close(&statement) == VPS_CLIENT_OK &&
                   vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "stmt_cancel_cleanup");
    return 1;
}

static int test_failures_and_fault_allocator(void)
{
    VpsAllocator system_allocator;
    VpsAllocator fault_allocator;
    VpsFaultAllocator fault;
    VpsClient client;
    VpsClientOperations operations;
    FakeBackend backend;
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;
    VpsClientPollResult result;
    VpsError error;
    TEST_CHECK(vps_allocator_system(&system_allocator) == VPS_MEMORY_OK &&
                   vps_error_init(&error, &system_allocator) == VPS_MEMORY_OK,
               "fail_setup");
    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    TEST_CHECK(vps_fault_allocator_init(&fault, &system_allocator, 1U) ==
                   VPS_MEMORY_OK &&
                   vps_fault_allocator_make(&fault, &fault_allocator) ==
                       VPS_MEMORY_OK &&
                   vps_client_init(&client, &fault_allocator, &operations,
                                   &backend, NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, &error) ==
                       VPS_CLIENT_OUT_OF_MEMORY && connection == NULL &&
                   backend.connection_create_count == 0U &&
                   fault.active_allocations == 0U &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "fail_alloc_connection");
    TEST_CHECK(vps_fault_allocator_reset(&fault, 0U) == VPS_MEMORY_OK,
               "fail_reset");

    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    TEST_CHECK(vps_fault_allocator_reset(&fault, 2U) == VPS_MEMORY_OK &&
                   vps_client_init(&client, &fault_allocator, &operations,
                                   &backend, NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, NULL) ==
                       VPS_CLIENT_OK && test_connect_ready(connection, &backend) &&
                   vps_client_statement_open(connection, &test_statement_spec,
                                             &statement, NULL) ==
                       VPS_CLIENT_OUT_OF_MEMORY && statement == NULL &&
                   backend.statement_create_count == 0U &&
                   vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   fault.active_allocations == 0U &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "fail_alloc_statement");
    TEST_CHECK(vps_fault_allocator_reset(&fault, 0U) == VPS_MEMORY_OK,
               "fail_statement_reset");

    fake_init(&backend);
    backend.connection_create_status = VPS_CLIENT_BACKEND_ERROR;
    backend.partial_connection = 1;
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    TEST_CHECK(vps_client_init(&client, &system_allocator, &operations,
                               &backend, NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, NULL) ==
                       VPS_CLIENT_BACKEND_ERROR && connection == NULL &&
                   backend.connection_destroy_count == 1U &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "fail_partial_connection");

    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL & ~VPS_CLIENT_CAP_PING);
    TEST_CHECK(vps_client_init(&client, &system_allocator, &operations,
                               &backend, NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, NULL) ==
                       VPS_CLIENT_OK && test_connect_ready(connection, &backend) &&
                   vps_client_connection_start(connection,
                                               VPS_CLIENT_OPERATION_PING,
                                               NULL) == VPS_CLIENT_UNSUPPORTED,
               "fail_missing_capability");
    backend.statement_create_status = VPS_CLIENT_BACKEND_ERROR;
    backend.partial_statement = 1;
    TEST_CHECK(vps_client_statement_open(connection, &test_statement_spec,
                                         &statement, NULL) ==
                   VPS_CLIENT_BACKEND_ERROR && statement == NULL &&
                   backend.statement_destroy_count == 1U,
               "fail_partial_statement");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "fail_partial_cleanup");

    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    TEST_CHECK(vps_client_init(&client, &system_allocator, &operations,
                               &backend, NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, NULL) ==
                       VPS_CLIENT_OK &&
                   vps_client_connection_start(connection,
                                               VPS_CLIENT_OPERATION_CONNECT,
                                               NULL) == VPS_CLIENT_OK,
               "fail_wait_setup");
    backend.connection_poll_result.outcome = VPS_CLIENT_POLL_WAIT;
    backend.connection_poll_result.wait.interest = VPS_CLIENT_WAIT_READ;
    backend.connection_poll_result.wait.phase = VPS_CLIENT_WAIT_CONNECT;
    backend.connection_poll_result.wait.max_slice_ms = 0U;
    TEST_CHECK(vps_client_connection_poll(connection, &result, NULL) ==
                   VPS_CLIENT_BACKEND_ERROR &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_FAILED,
               "fail_invalid_wait");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "fail_wait_cleanup");

    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    TEST_CHECK(vps_client_init(&client, &system_allocator, &operations,
                               &backend, NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, NULL) ==
                       VPS_CLIENT_OK &&
                   vps_client_connection_start(connection,
                                               VPS_CLIENT_OPERATION_CONNECT,
                                               NULL) == VPS_CLIENT_OK,
               "fail_timeout_setup");
    backend.connection_poll_result.outcome = VPS_CLIENT_POLL_FAILED;
    backend.poll_error_class = VPS_ERROR_CLASS_TIMEOUT;
    TEST_CHECK(vps_client_connection_poll(connection, &result, &error) ==
                   VPS_CLIENT_BACKEND_ERROR &&
                   error.error_class == VPS_ERROR_CLASS_TIMEOUT,
               "fail_timeout");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "fail_timeout_cleanup");

    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    TEST_CHECK(vps_client_init(&client, &system_allocator, &operations,
                               &backend, NULL) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, NULL) ==
                       VPS_CLIENT_OK &&
                   vps_client_connection_start(connection,
                                               VPS_CLIENT_OPERATION_CONNECT,
                                               NULL) == VPS_CLIENT_OK,
               "fail_interrupt_setup");
    backend.connection_poll_result.outcome = VPS_CLIENT_POLL_FAILED;
    backend.poll_error_class = VPS_ERROR_CLASS_CANCEL;
    TEST_CHECK(vps_client_connection_poll(connection, &result, &error) ==
                   VPS_CLIENT_BACKEND_ERROR &&
                   error.error_class == VPS_ERROR_CLASS_CANCEL,
               "fail_interrupt");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "fail_interrupt_cleanup");
    vps_error_reset(&error);
    return 1;
}

static int test_logging_and_names(void)
{
    VpsAllocator allocator;
    VpsClient client;
    VpsClientOperations operations;
    FakeBackend backend;
    TestLogCapture capture;
    VpsLogger logger;
    VpsClientConnection *connection = NULL;
    (void)memset(&capture, 0, sizeof(capture));
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG,
                                   test_log_sink, &capture) == VPS_LOG_OK,
               "log_setup");
    fake_init(&backend);
    operations = fake_operations(VPS_CLIENT_CAP_ALL);
    TEST_CHECK(vps_client_init(&client, &allocator, &operations, &backend,
                               &logger) == VPS_CLIENT_OK &&
                   vps_client_connection_open(&client, &connection, NULL) ==
                       VPS_CLIENT_OK && test_connect_ready(connection, &backend) &&
                   vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   vps_client_cleanup(&client) == VPS_CLIENT_OK,
               "log_lifecycle");
    TEST_CHECK(capture.event_count > 0U &&
                   capture.operation_count == capture.event_count &&
                   capture.phase_count == capture.event_count &&
                   capture.status_count == capture.event_count &&
                   !capture.forbidden_seen,
               "log_fields");
    TEST_CHECK(strcmp(vps_client_operation_name(VPS_CLIENT_OPERATION_FETCH),
                      "fetch") == 0 &&
                   strcmp(vps_client_poll_outcome_name(VPS_CLIENT_POLL_WAIT),
                          "wait") == 0 &&
                   strcmp(vps_client_wait_phase_name(VPS_CLIENT_WAIT_CANCEL),
                          "cancel") == 0,
               "log_names");
    return 1;
}

int main(void)
{
    if (!test_abi_and_lifecycle() ||
        !test_connection_machine() ||
        !test_statement_rows_and_cancel() ||
        !test_failures_and_fault_allocator() ||
        !test_logging_and_names()) {
        return 1;
    }
    (void)fprintf(stdout, "vps_client_test status=passed\n");
    return 0;
}
