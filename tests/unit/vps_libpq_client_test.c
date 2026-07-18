#include "vps_libpq_client.h"

#include <stdio.h>
#include <string.h>

#define TEST_CHECK(condition, name)                                        \
    do {                                                                   \
        if (!(condition)) {                                                \
            (void)fprintf(stderr, "libpq_client_case=%s status=failed\n", \
                          (name));                                         \
            return 0;                                                      \
        }                                                                  \
    } while (0)

#define FAKE_POLL_LIMIT 16U

typedef struct FakePlatform {
    uint64_t now_ms;
    size_t wait_count;
    intptr_t observed_socket[FAKE_POLL_LIMIT];
    VpsWaitInterest observed_interest[FAKE_POLL_LIMIT];
    VpsPlatformStatus wait_status;
} FakePlatform;

typedef struct FakeApi {
    int connection_token;
    int result_token;
    int cancel_token;
    size_t start_count;
    size_t nonblocking_count;
    size_t poll_count;
    size_t socket_count;
    size_t finish_count;
    size_t tls_count;
    size_t identity_count;
    size_t send_prepare_count;
    size_t send_describe_count;
    size_t send_execute_count;
    size_t flush_count;
    size_t consume_count;
    size_t clear_count;
    size_t single_row_count;
    size_t readiness_count;
    size_t cancel_create_count;
    size_t cancel_start_count;
    size_t cancel_poll_count;
    size_t cancel_finish_count;
    int readiness_order[3];
    int keyword_view_valid;
    int return_null_connection;
    int nonblocking_result;
    int identity_result;
    VpsErrorClass error_class;
    VpsTlsResult tls_result;
    VpsLibpqConnectionStatus connection_status;
    VpsLibpqTransactionStatus transaction_status;
    VpsLibpqPipelineStatus pipeline_status;
    VpsLibpqPollingStatus polls[FAKE_POLL_LIMIT];
    VpsLibpqPollingStatus cancel_polls[FAKE_POLL_LIMIT];
    size_t cancel_poll_length;
    intptr_t cancel_socket;
    intptr_t sockets[FAKE_POLL_LIMIT];
    size_t poll_length;
    size_t last_poll_index;
    int current_command;
    int get_result_step;
    int flush_would_block_once;
    int busy_once;
    int send_result;
    VpsLibpqResultStatus forced_result_status;
    int described_parameter_count;
    uint32_t described_parameter_types[8];
    int described_field_count;
    uint32_t described_field_types[8];
    int described_field_formats[8];
    const char *described_field_names[8];
    int32_t described_field_modifiers[8];
    uint32_t described_field_relations[8];
    int32_t described_field_attributes[8];
    const char *result_sqlstate;
    const char *result_primary_message;
    const char *command_tuples;
    VpsLibpqResultStatus stream_statuses[8];
    size_t stream_length;
    size_t stream_index;
    size_t last_stream_index;
    char health_value[VPS_SESSION_SEARCH_PATH_LIMIT + 1U];
    int health_mismatch;
} FakeApi;

typedef struct TestFixture {
    VpsAllocator allocator;
    VpsConnectionConfig config;
    VpsConnectionIdentity identity;
    VpsTlsPolicy tls_policy;
    VpsSessionPlan session_plan;
    VpsLibpqClientApi api;
    VpsLibpqClient adapter;
    VpsClient client;
    FakeApi fake;
    VpsError error;
    VpsLogger logger;
    size_t log_count;
    int forbidden_seen;
    int primary_message_seen;
    int sql_text_seen;
} TestFixture;

static FakePlatform *vps_test_platform_context;

static VpsPlatformStatus fake_now(uint64_t *milliseconds)
{
    if (vps_test_platform_context == NULL || milliseconds == NULL) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    *milliseconds = vps_test_platform_context->now_ms;
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus fake_socket_wait(intptr_t socket_handle,
                                          VpsWaitInterest interest,
                                          uint32_t timeout_ms,
                                          VpsWaitInterest *ready_interest)
{
    FakePlatform *platform = vps_test_platform_context;
    size_t index;
    if (platform == NULL || ready_interest == NULL || timeout_ms == 0U) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    index = platform->wait_count;
    if (index < FAKE_POLL_LIMIT) {
        platform->observed_socket[index] = socket_handle;
        platform->observed_interest[index] = interest;
    }
    platform->wait_count += 1U;
    if (platform->wait_status == VPS_PLATFORM_TIMEOUT) {
        platform->now_ms += timeout_ms;
        *ready_interest = (VpsWaitInterest)0;
    } else if (platform->wait_status == VPS_PLATFORM_OK) {
        *ready_interest = interest;
    } else {
        *ready_interest = (VpsWaitInterest)0;
    }
    return platform->wait_status;
}

static VpsPlatformOperations fake_platform_operations(FakePlatform *platform)
{
    VpsPlatformOperations operations;
    (void)memset(platform, 0, sizeof(*platform));
    platform->now_ms = 1000U;
    platform->wait_status = VPS_PLATFORM_OK;
    (void)memset(&operations, 0, sizeof(operations));
    operations.structure_size = sizeof(operations);
    operations.contract_version = VPS_PLATFORM_CONTRACT_VERSION;
    operations.capabilities = VPS_PLATFORM_CAP_MONOTONIC_CLOCK |
                              VPS_PLATFORM_CAP_SOCKET_WAIT;
    operations.monotonic_now_ms = fake_now;
    operations.socket_wait = fake_socket_wait;
    vps_test_platform_context = platform;
    return operations;
}

static int fake_keyword_equals(const char *const *keywords,
                               const char *const *values,
                               const char *keyword,
                               const char *value)
{
    size_t index;
    for (index = 0U; keywords[index] != NULL; ++index) {
        if (strcmp(keywords[index], keyword) == 0) {
            return values[index] != NULL && strcmp(values[index], value) == 0;
        }
    }
    return 0;
}

static void *fake_connect_start(void *context,
                                const char *const *keywords,
                                const char *const *values,
                                int expand_dbname)
{
    FakeApi *fake = (FakeApi *)context;
    size_t keyword_count = 0U;
    fake->start_count += 1U;
    while (keyword_count < VPS_LIBPQ_CLIENT_MAX_KEYWORDS &&
           keywords[keyword_count] != NULL) {
        keyword_count += 1U;
    }
    fake->keyword_view_valid =
        expand_dbname == 0 && keyword_count > 0U &&
        keyword_count < VPS_LIBPQ_CLIENT_MAX_KEYWORDS &&
        values[keyword_count] == NULL &&
        fake_keyword_equals(keywords, values, "host", "db.invalid") &&
        fake_keyword_equals(keywords, values, "password",
                            "synthetic-secret") &&
        fake_keyword_equals(keywords, values, "sslmode", "verify-full") &&
        fake_keyword_equals(keywords, values, "channel_binding", "require") &&
        fake_keyword_equals(keywords, values, "client_encoding", "UTF8");
    return fake->return_null_connection ? NULL : &fake->connection_token;
}

static int fake_set_nonblocking(void *context,
                                void *connection,
                                int enabled)
{
    FakeApi *fake = (FakeApi *)context;
    if (connection != &fake->connection_token || enabled != 1) return 1;
    fake->nonblocking_count += 1U;
    return fake->nonblocking_result;
}

static VpsLibpqPollingStatus fake_connect_poll(void *context,
                                                void *connection)
{
    FakeApi *fake = (FakeApi *)context;
    size_t index;
    if (connection != &fake->connection_token || fake->poll_length == 0U) {
        return VPS_LIBPQ_POLL_FAILED;
    }
    index = fake->poll_count < fake->poll_length
                ? fake->poll_count
                : fake->poll_length - 1U;
    fake->last_poll_index = index;
    fake->poll_count += 1U;
    return fake->polls[index];
}

static intptr_t fake_socket_handle(void *context, const void *connection)
{
    FakeApi *fake = (FakeApi *)context;
    if (connection != &fake->connection_token) return (intptr_t)-1;
    fake->socket_count += 1U;
    return fake->sockets[fake->last_poll_index];
}

static VpsLibpqConnectionStatus fake_connection_status(
    void *context,
    const void *connection)
{
    FakeApi *fake = (FakeApi *)context;
    return connection == &fake->connection_token
               ? fake->connection_status
               : VPS_LIBPQ_CONNECTION_BAD;
}

static VpsLibpqTransactionStatus fake_transaction_status(
    void *context, const void *connection)
{
    FakeApi *fake = (FakeApi *)context;
    return connection == &fake->connection_token
               ? fake->transaction_status
               : VPS_LIBPQ_TRANSACTION_UNKNOWN;
}

static VpsLibpqPipelineStatus fake_pipeline_status(
    void *context, const void *connection)
{
    FakeApi *fake = (FakeApi *)context;
    return connection == &fake->connection_token
               ? fake->pipeline_status
               : VPS_LIBPQ_PIPELINE_UNKNOWN;
}

static VpsErrorClass fake_error_class(void *context,
                                      const void *connection)
{
    FakeApi *fake = (FakeApi *)context;
    return connection == &fake->connection_token
               ? fake->error_class
               : VPS_ERROR_CLASS_CONNECTION;
}

static VpsTlsResult fake_tls_verify(void *context,
                                    void *connection,
                                    const VpsTlsPolicy *policy,
                                    VpsTlsOutcome *outcome,
                                    VpsLogger *logger)
{
    FakeApi *fake = (FakeApi *)context;
    (void)logger;
    if (connection != &fake->connection_token || policy == NULL ||
        outcome == NULL) return VPS_TLS_INVALID_ARGUMENT;
    fake->tls_count += 1U;
    fake->readiness_order[fake->readiness_count++] = 1;
    (void)memset(outcome, 0, sizeof(*outcome));
    outcome->mode = policy->mode;
    outcome->ssl_in_use = 1;
    return fake->tls_result;
}

static int fake_identity_verify(void *context,
                                const void *connection,
                                const VpsConnectionIdentity *identity)
{
    FakeApi *fake = (FakeApi *)context;
    if (connection != &fake->connection_token || identity == NULL) return 0;
    fake->identity_count += 1U;
    fake->readiness_order[fake->readiness_count++] = 2;
    return fake->identity_result;
}

static int fake_send_prepare(void *context, void *connection,
                             const char *name, const char *query,
                             int count, const uint32_t *types)
{
    FakeApi *fake = (FakeApi *)context;
    (void)connection; (void)name; (void)query; (void)count; (void)types;
    fake->send_prepare_count += 1U;
    fake->current_command = 1;
    fake->get_result_step = 0;
    return fake->send_result;
}

static int fake_send_describe(void *context, void *connection,
                              const char *name)
{
    FakeApi *fake = (FakeApi *)context;
    (void)connection; (void)name;
    fake->send_describe_count += 1U;
    fake->current_command = 2;
    fake->get_result_step = 0;
    return fake->send_result;
}

static int fake_send_prepared(void *context, void *connection,
                              const char *name, int count,
                              const char *const *values, const int *lengths,
                              const int *formats, int result_format)
{
    FakeApi *fake = (FakeApi *)context;
    (void)connection; (void)name; (void)count; (void)values;
    (void)lengths; (void)formats; (void)result_format;
    fake->send_execute_count += 1U;
    fake->current_command = 3;
    fake->get_result_step = 0;
    fake->stream_index = 0U;
    return fake->send_result;
}

static int fake_send_params(void *context, void *connection,
                            const char *query, int count,
                            const uint32_t *types,
                            const char *const *values, const int *lengths,
                            const int *formats, int result_format)
{
    FakeApi *fake = (FakeApi *)context;
    (void)connection; (void)types;
    (void)lengths; (void)formats; (void)result_format;
    fake->send_execute_count += 1U;
    if (strcmp(query, "SELECT pg_catalog.set_config($1, $2, false)") == 0) {
        fake->current_command = 4;
        if (count == 2 && values != NULL && values[1] != NULL) {
            size_t length = strlen(values[1]);
            if (length >= sizeof(fake->health_value)) {
                return 0;
            }
            (void)memcpy(fake->health_value, values[1], length + 1U);
        }
    } else if (strcmp(query, "SELECT 1") == 0) {
        fake->current_command = 5;
    } else if (strcmp(query, "DISCARD ALL") == 0) {
        fake->current_command = 6;
    } else if (strcmp(query, "BEGIN") == 0) {
        fake->current_command = 7;
    } else if (strcmp(query, "COMMIT") == 0) {
        fake->current_command = 8;
    } else if (strcmp(query, "ROLLBACK") == 0) {
        fake->current_command = 9;
    } else {
        fake->current_command = 3;
    }
    fake->get_result_step = 0;
    fake->stream_index = 0U;
    return fake->send_result;
}

static int fake_flush(void *context, void *connection)
{
    FakeApi *fake = (FakeApi *)context;
    (void)connection;
    fake->flush_count += 1U;
    if (fake->flush_would_block_once) {
        fake->flush_would_block_once = 0;
        return 1;
    }
    return 0;
}
static int fake_set_single_row(void *context, void *connection)
{ FakeApi *fake = (FakeApi *)context; (void)connection;
  fake->single_row_count += 1U; return 1; }
static void *fake_cancel_create(void *context, void *connection)
{ FakeApi *fake = (FakeApi *)context;
  if (connection != &fake->connection_token) return NULL;
  fake->cancel_create_count += 1U; return &fake->cancel_token; }
static int fake_cancel_start(void *context, void *cancel_connection)
{ FakeApi *fake = (FakeApi *)context;
  if (cancel_connection != &fake->cancel_token) return 0;
  fake->cancel_start_count += 1U; fake->cancel_poll_count = 0U; return 1; }
static VpsLibpqPollingStatus fake_cancel_poll(void *context,
                                              void *cancel_connection)
{ FakeApi *fake = (FakeApi *)context; size_t index;
  if (cancel_connection != &fake->cancel_token ||
      fake->cancel_poll_length == 0U) return VPS_LIBPQ_POLL_FAILED;
  index = fake->cancel_poll_count < fake->cancel_poll_length
              ? fake->cancel_poll_count
              : fake->cancel_poll_length - 1U;
  fake->cancel_poll_count += 1U; return fake->cancel_polls[index]; }
static intptr_t fake_cancel_socket(void *context,
                                   const void *cancel_connection)
{ FakeApi *fake = (FakeApi *)context;
  return cancel_connection == &fake->cancel_token ? fake->cancel_socket
                                                   : (intptr_t)-1; }
static void fake_cancel_reset(void *context, void *cancel_connection)
{ (void)context; (void)cancel_connection; }
static void fake_cancel_finish(void *context, void *cancel_connection)
{ FakeApi *fake = (FakeApi *)context;
  if (cancel_connection == &fake->cancel_token) fake->cancel_finish_count += 1U; }
static int fake_consume(void *context, void *connection)
{ FakeApi *fake = (FakeApi *)context; (void)connection;
  fake->consume_count += 1U; return 1; }
static int fake_busy(void *context, const void *connection)
{
    FakeApi *fake = (FakeApi *)context;
    (void)connection;
    if (fake->busy_once) { fake->busy_once = 0; return 1; }
    return 0;
}
static void *fake_get_result(void *context, void *connection)
{
    FakeApi *fake = (FakeApi *)context;
    (void)connection;
    if (fake->current_command == 0) return NULL;
    if (fake->current_command == 3 && fake->stream_length != 0U) {
        if (fake->stream_index >= fake->stream_length) return NULL;
        fake->last_stream_index = fake->stream_index++;
        return &fake->result_token;
    }
    if (fake->get_result_step++ == 0) return &fake->result_token;
    if (fake->current_command == 7)
        fake->transaction_status = VPS_LIBPQ_TRANSACTION_ACTIVE;
    else if (fake->current_command == 8 || fake->current_command == 9)
        fake->transaction_status = VPS_LIBPQ_TRANSACTION_IDLE;
    return NULL;
}

static VpsLibpqResultStatus fake_result_status(void *context,
                                               const void *result)
{
    FakeApi *fake = (FakeApi *)context;
    (void)result;
    if (fake->forced_result_status != VPS_LIBPQ_RESULT_EMPTY) {
        return fake->forced_result_status;
    }
    if (fake->current_command == 3 && fake->stream_length != 0U) {
        return fake->stream_statuses[fake->last_stream_index];
    }
    return fake->current_command == 3 || fake->current_command == 4 ||
                   fake->current_command == 5
               ? VPS_LIBPQ_RESULT_TUPLES_OK
               : VPS_LIBPQ_RESULT_COMMAND_OK;
}
static int fake_result_parameter_count(void *context, const void *result)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->described_parameter_count; }
static int fake_result_field_count(void *context, const void *result)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->current_command == 4 || fake->current_command == 5
             ? 1
             : fake->described_field_count; }
static uint32_t fake_result_type(void *context, const void *result, int index)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->current_command == 2 ? fake->described_parameter_types[index]
                                    : fake->described_field_types[index]; }
static uint32_t fake_result_field_type(void *context, const void *result,
                                       int index)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->described_field_types[index]; }
static const char *fake_result_field_name(void *context, const void *result,
                                          int index)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->described_field_names[index] != NULL
             ? fake->described_field_names[index] : "value"; }
static int32_t fake_result_field_modifier(void *context, const void *result,
                                          int index)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->described_field_modifiers[index]; }
static uint32_t fake_result_field_relation(void *context, const void *result,
                                           int index)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->described_field_relations[index]; }
static int32_t fake_result_field_attribute(void *context, const void *result,
                                           int index)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->described_field_attributes[index]; }
static int fake_result_format(void *context, const void *result, int index)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->described_field_formats[index]; }
static int fake_result_row_count(void *context, const void *result)
{ FakeApi *fake = (FakeApi *)context;
  return fake_result_status(context, result) == VPS_LIBPQ_RESULT_SINGLE_TUPLE ||
                 fake->current_command == 4 || fake->current_command == 5
             ? 1 : 0; }
static const char *fake_result_command_tuples(void *context,
                                               const void *result)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->command_tuples != NULL ? fake->command_tuples : ""; }
static int fake_result_is_null(void *context, const void *result,
                               int row, int column)
{ (void)context; (void)result; (void)row; (void)column; return 0; }
static const void *fake_result_value(void *context, const void *result,
                                     int row, int column)
{ static const char value[] = "1"; static const char mismatch[] = "mismatch";
  FakeApi *fake = (FakeApi *)context; (void)result; (void)row; (void)column;
  if (fake->current_command == 4) {
      return fake->health_mismatch ? mismatch : fake->health_value;
  }
  return value; }
static int fake_result_value_length(void *context, const void *result,
                                    int row, int column)
{ FakeApi *fake = (FakeApi *)context; (void)result; (void)row; (void)column;
  return fake->current_command == 4
             ? (int)strlen((const char *)fake_result_value(context, result,
                                                           row, column))
             : 1; }
static const char *fake_result_sqlstate(void *context, const void *result)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->result_sqlstate; }
static const char *fake_result_primary_message(void *context,
                                               const void *result)
{ FakeApi *fake = (FakeApi *)context; (void)result;
  return fake->result_primary_message; }
static void fake_clear_result(void *context, void *result)
{ FakeApi *fake = (FakeApi *)context; (void)result; fake->clear_count += 1U; }

static void fake_finish(void *context, void *connection)
{
    FakeApi *fake = (FakeApi *)context;
    if (connection == &fake->connection_token) fake->finish_count += 1U;
}

static int test_log_sink(void *context, const VpsLogEvent *event)
{
    static const char forbidden[] = "synthetic-secret";
    TestFixture *fixture = (TestFixture *)context;
    size_t index;
    fixture->log_count += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            field->value.string_value.length == sizeof(forbidden) - 1U &&
            memcmp(field->value.string_value.data, forbidden,
                   sizeof(forbidden) - 1U) == 0) {
            fixture->forbidden_seen = 1;
        }
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            field->key == VPS_LOG_FIELD_PRIMARY_MESSAGE &&
            field->value.string_value.length == 23U &&
            memcmp(field->value.string_value.data,
                   "division by zero near ?", 23U) == 0) {
            fixture->primary_message_seen = 1;
        }
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            field->key == VPS_LOG_FIELD_SQL_TEXT) {
            fixture->sql_text_seen = 1;
        }
    }
    return 0;
}

static VpsLibpqClientApi fake_api(FakeApi *fake)
{
    VpsLibpqClientApi api;
    (void)memset(fake, 0, sizeof(*fake));
    fake->identity_result = 1;
    fake->error_class = VPS_ERROR_CLASS_CONNECTION;
    fake->tls_result = VPS_TLS_OK;
    fake->connection_status = VPS_LIBPQ_CONNECTION_OK;
    fake->transaction_status = VPS_LIBPQ_TRANSACTION_IDLE;
    fake->pipeline_status = VPS_LIBPQ_PIPELINE_OFF;
    fake->send_result = 1;
    fake->cancel_socket = 77;
    (void)memset(&api, 0, sizeof(api));
    api.structure_size = sizeof(api);
    api.api_version = VPS_LIBPQ_CLIENT_API_VERSION;
    api.context = fake;
    api.connect_start_params = fake_connect_start;
    api.set_nonblocking = fake_set_nonblocking;
    api.connect_poll = fake_connect_poll;
    api.socket_handle = fake_socket_handle;
    api.connection_status = fake_connection_status;
    api.transaction_status = fake_transaction_status;
    api.pipeline_status = fake_pipeline_status;
    api.connection_error_class = fake_error_class;
    api.tls_verify = fake_tls_verify;
    api.identity_verify = fake_identity_verify;
    api.send_prepare = fake_send_prepare;
    api.send_describe_prepared = fake_send_describe;
    api.send_query_prepared = fake_send_prepared;
    api.send_query_params = fake_send_params;
    api.set_single_row_mode = fake_set_single_row;
    api.cancel_create = fake_cancel_create;
    api.cancel_start = fake_cancel_start;
    api.cancel_poll = fake_cancel_poll;
    api.cancel_socket = fake_cancel_socket;
    api.cancel_reset = fake_cancel_reset;
    api.cancel_finish = fake_cancel_finish;
    api.flush = fake_flush;
    api.consume_input = fake_consume;
    api.is_busy = fake_busy;
    api.get_result = fake_get_result;
    api.result_status = fake_result_status;
    api.result_parameter_count = fake_result_parameter_count;
    api.result_parameter_type = fake_result_type;
    api.result_field_count = fake_result_field_count;
    api.result_field_type = fake_result_field_type;
    api.result_field_name = fake_result_field_name;
    api.result_field_modifier = fake_result_field_modifier;
    api.result_field_relation = fake_result_field_relation;
    api.result_field_attribute = fake_result_field_attribute;
    api.result_field_format = fake_result_format;
    api.result_row_count = fake_result_row_count;
    api.result_value_is_null = fake_result_is_null;
    api.result_value = fake_result_value;
    api.result_value_length = fake_result_value_length;
    api.result_sqlstate = fake_result_sqlstate;
    api.clear_result = fake_clear_result;
    api.finish = fake_finish;
    api.result_primary_message = fake_result_primary_message;
    api.result_command_tuples = fake_result_command_tuples;
    return api;
}

static int test_fixture_init(TestFixture *fixture,
                             FakePlatform *platform,
                             VpsPlatformOperations *platform_operations,
                             const VpsAllocator *adapter_allocator)
{
    VpsLibpqClientOptions options;
    VpsClientOperations operations;
    size_t index;
    (void)memset(fixture, 0, sizeof(*fixture));
    if (vps_allocator_system(&fixture->allocator) != VPS_MEMORY_OK) return 0;
    *platform_operations = fake_platform_operations(platform);
    fixture->api = fake_api(&fixture->fake);
    fixture->config.initialized = 1;
    fixture->config.config.header.structure_size =
        (uint32_t)sizeof(fixture->config.config);
    fixture->config.config.header.api_version = VPS_API_VERSION;
    fixture->config.config.header.present_fields =
        VPS_CREDENTIAL_FIELD_HOSTS | VPS_CREDENTIAL_FIELD_USER |
        VPS_CREDENTIAL_FIELD_PASSWORD | VPS_CREDENTIAL_FIELD_DBNAME;
    fixture->config.config.hosts = "db.invalid";
    fixture->config.config.user = "synthetic-user";
    fixture->config.config.password = "synthetic-secret";
    fixture->config.config.dbname = "synthetic-db";
    fixture->identity.initialized = 1;
    fixture->identity.built = 1;
    for (index = 0U; index < VPS_IDENTITY_FINGERPRINT_LENGTH; ++index) {
        fixture->identity.fingerprint[index] = 'a';
    }
    fixture->identity.fingerprint[VPS_IDENTITY_FINGERPRINT_LENGTH] = '\0';
    fixture->tls_policy.initialized = 1;
    fixture->tls_policy.mode = VPS_TLS_MODE_VERIFY_FULL;
    fixture->tls_policy.channel_binding = VPS_CHANNEL_BINDING_REQUIRE;
    fixture->session_plan.initialized = 1;
    fixture->session_plan.built = 1;
    if (vps_logger_init(&fixture->logger, VPS_LOG_LEVEL_DEBUG, test_log_sink,
                        fixture) != VPS_LOG_OK ||
        vps_error_init(&fixture->error, &fixture->allocator) != VPS_MEMORY_OK) {
        return 0;
    }
    (void)memset(&options, 0, sizeof(options));
    options.allocator = adapter_allocator == NULL ? &fixture->allocator
                                                  : adapter_allocator;
    options.platform_operations = platform_operations;
    options.connection_config = &fixture->config;
    options.identity = &fixture->identity;
    options.tls_policy = &fixture->tls_policy;
    options.session_plan = &fixture->session_plan;
    options.logger = &fixture->logger;
    options.connect_timeout_ms = 100U;
    options.wait_slice_ms = 10U;
    options.api = &fixture->api;
    if (vps_libpq_client_init(&fixture->adapter, &options) != VPS_CLIENT_OK ||
        vps_libpq_client_make_operations(&fixture->adapter, &operations) !=
            VPS_CLIENT_OK ||
        vps_client_init(&fixture->client, &fixture->allocator, &operations,
                        &fixture->adapter, &fixture->logger) != VPS_CLIENT_OK) {
        return 0;
    }
    return 1;
}

static int test_fixture_cleanup(TestFixture *fixture)
{
    int clean = vps_client_cleanup(&fixture->client) == VPS_CLIENT_OK &&
                vps_libpq_client_cleanup(&fixture->adapter) == VPS_CLIENT_OK;
    vps_error_reset(&fixture->error);
    return clean;
}

static VpsClientStatus test_drive(VpsClientConnection *connection,
                                  VpsError *error)
{
    VpsClientPollResult result;
    size_t attempts;
    for (attempts = 0U; attempts < 32U; ++attempts) {
        VpsClientStatus status = vps_client_connection_poll(
            connection, &result, error);
        if (status != VPS_CLIENT_OK) return status;
        if (result.outcome == VPS_CLIENT_POLL_COMPLETE) return VPS_CLIENT_OK;
        if (result.outcome != VPS_CLIENT_POLL_WAIT) {
            return VPS_CLIENT_BACKEND_ERROR;
        }
        status = vps_client_connection_wait(connection, &result.wait, error);
        if (status != VPS_CLIENT_OK) return status;
    }
    return VPS_CLIENT_LIMIT_EXCEEDED;
}

static VpsClientStatus test_drive_statement(VpsClientStatement *statement,
                                             VpsError *error)
{
    VpsClientPollResult result;
    size_t attempts;
    for (attempts = 0U; attempts < 64U; ++attempts) {
        VpsClientStatus status = vps_client_statement_poll(
            statement, &result, error);
        if (status != VPS_CLIENT_OK) return status;
        if (result.outcome == VPS_CLIENT_POLL_COMPLETE ||
            result.outcome == VPS_CLIENT_POLL_ROW_READY) return VPS_CLIENT_OK;
        if (result.outcome != VPS_CLIENT_POLL_WAIT) {
            return VPS_CLIENT_BACKEND_ERROR;
        }
        status = vps_client_statement_wait(statement, &result.wait, error);
        if (status != VPS_CLIENT_OK) return status;
    }
    return VPS_CLIENT_LIMIT_EXCEEDED;
}

static int test_statement_prepare_describe_execute(void)
{
    static const char query[] =
        "SELECT $1::int4 WHERE $2::bytea IS NOT NULL";
    static const char int_value[] = "7";
    static const unsigned char binary_value[] = {0x00U, 0xffU};
    VpsClientParameterView parameters[2];
    VpsClientResultFieldExpectation fields[1];
    VpsClientStatementSpec spec;
    VpsClientStatementMetadata metadata;
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "statement_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.sockets[0] = 41;
    fixture.fake.poll_length = 1U;
    fixture.fake.flush_would_block_once = 1;
    fixture.fake.busy_once = 1;
    fixture.fake.described_parameter_count = 2;
    fixture.fake.described_parameter_types[0] = 23U;
    fixture.fake.described_parameter_types[1] = 17U;
    fixture.fake.described_field_count = 1;
    fixture.fake.described_field_types[0] = 23U;
    fixture.fake.described_field_formats[0] = 0;
    (void)memset(parameters, 0, sizeof(parameters));
    parameters[0].value = int_value;
    parameters[0].length = sizeof(int_value) - 1U;
    parameters[0].type_oid = 23U;
    parameters[0].format = VPS_CLIENT_VALUE_TEXT;
    parameters[1].value = binary_value;
    parameters[1].length = sizeof(binary_value);
    parameters[1].type_oid = 17U;
    parameters[1].format = VPS_CLIENT_VALUE_BINARY;
    fields[0].type_oid = 23U;
    fields[0].format = VPS_CLIENT_VALUE_TEXT;
    (void)memset(&spec, 0, sizeof(spec));
    spec.query = query;
    spec.query_length = sizeof(query) - 1U;
    spec.parameters = parameters;
    spec.parameter_count = 2U;
    spec.result_fields = fields;
    spec.result_field_count = 1U;
    spec.timeout_ms = 100U;
    spec.prepare = 1;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK,
               "statement_connect");
    TEST_CHECK(vps_client_statement_open(connection, &spec, &statement,
                                         &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_PREPARE,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_OK &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_PREPARED,
               "statement_prepare");
    TEST_CHECK(vps_client_statement_metadata(statement, &metadata,
                                              &fixture.error) ==
                       VPS_CLIENT_OK &&
                   metadata.parameter_count == 2U &&
                   metadata.result_field_count == 1U && metadata.described &&
                   metadata.query_fingerprint != 0U,
               "statement_metadata");
    TEST_CHECK(vps_client_statement_start(
                   statement, VPS_CLIENT_OPERATION_EXECUTE,
                   &fixture.error) == VPS_CLIENT_OK &&
                   test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_OK &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_FETCHING,
               "statement_execute");
    TEST_CHECK(fixture.fake.send_prepare_count == 1U &&
                   fixture.fake.send_describe_count == 1U &&
                   fixture.fake.send_execute_count == 1U &&
                   fixture.fake.clear_count == 3U && platform.wait_count == 2U &&
                   !fixture.forbidden_seen,
               "statement_counts");
    TEST_CHECK(vps_client_statement_close(&statement) == VPS_CLIENT_OK &&
                   vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   fixture.fake.finish_count == 1U &&
                   test_fixture_cleanup(&fixture),
               "statement_cleanup");
    return 1;
}

static int test_statement_metadata_mismatch(void)
{
    static const char query[] = "SELECT 1::int4";
    VpsClientResultFieldExpectation field;
    VpsClientStatementSpec spec;
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "mismatch_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.sockets[0] = 42;
    fixture.fake.poll_length = 1U;
    fixture.fake.described_field_count = 1;
    fixture.fake.described_field_types[0] = 25U;
    field.type_oid = 23U;
    field.format = VPS_CLIENT_VALUE_TEXT;
    (void)memset(&spec, 0, sizeof(spec));
    spec.query = query;
    spec.query_length = sizeof(query) - 1U;
    spec.result_fields = &field;
    spec.result_field_count = 1U;
    spec.timeout_ms = 100U;
    spec.prepare = 1;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_open(connection, &spec, &statement,
                                             &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_PREPARE,
                       &fixture.error) == VPS_CLIENT_OK,
               "mismatch_start");
    TEST_CHECK(test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   fixture.error.error_class == VPS_ERROR_CLASS_METADATA &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_FAILED &&
                   fixture.fake.clear_count == 2U,
               "mismatch_detected");
    TEST_CHECK(vps_client_statement_close(&statement) == VPS_CLIENT_OK &&
                   vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "mismatch_cleanup");
    return 1;
}

static int test_single_row_and_late_error(void)
{
    static const char query[] = "SELECT 1::int4";
    VpsClientResultFieldExpectation field;
    VpsClientStatementSpec spec;
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;
    const VpsClientRowView *row = NULL;
    VpsClientColumnView column;
    VpsClientStatementMetadata metadata;
    uint64_t first_generation = 0U;
    size_t index;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "stream_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.sockets[0] = 43;
    fixture.fake.poll_length = 1U;
    fixture.fake.described_field_count = 1;
    fixture.fake.described_field_types[0] = 23U;
    fixture.fake.described_field_formats[0] = 0;
    fixture.fake.stream_statuses[0] = VPS_LIBPQ_RESULT_SINGLE_TUPLE;
    fixture.fake.stream_statuses[1] = VPS_LIBPQ_RESULT_SINGLE_TUPLE;
    fixture.fake.stream_statuses[2] = VPS_LIBPQ_RESULT_TUPLES_OK;
    fixture.fake.stream_length = 3U;
    fixture.fake.command_tuples = "2";
    field.type_oid = 23U;
    field.format = VPS_CLIENT_VALUE_TEXT;
    (void)memset(&spec, 0, sizeof(spec));
    spec.query = query;
    spec.query_length = sizeof(query) - 1U;
    spec.result_fields = &field;
    spec.result_field_count = 1U;
    spec.timeout_ms = 100U;
    spec.single_row = 1;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_open(connection, &spec, &statement,
                                             &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_EXECUTE,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_OK,
               "stream_start");
    for (index = 0U; index < 2U; ++index) {
        row = NULL;
        TEST_CHECK(vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_FETCH,
                       &fixture.error) == VPS_CLIENT_OK,
                   "stream_fetch_start");
        TEST_CHECK(test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_OK,
                   "stream_fetch_drive");
        TEST_CHECK(vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_ROW_READY,
                   "stream_row_state");
        TEST_CHECK(vps_client_statement_current_row(
                       statement, &row, &fixture.error) == VPS_CLIENT_OK,
                   "stream_row_current");
        TEST_CHECK(vps_client_row_column_count(row) == 1U,
                   "stream_row_count");
        TEST_CHECK(vps_client_row_column(row, 0U, &column,
                                         &fixture.error) == VPS_CLIENT_OK,
                   "stream_row_column");
        TEST_CHECK(!column.is_null && column.length == 1U,
                   "stream_row_value");
        if (index == 0U) {
            first_generation = vps_client_statement_row_generation(statement);
        }
        TEST_CHECK(vps_client_statement_row_consumed(
                       statement, &fixture.error) == VPS_CLIENT_OK,
                   "stream_consume");
    }
    TEST_CHECK(!vps_client_statement_row_is_current(statement,
                                                     first_generation) &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_FETCH,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_OK &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_COMPLETE &&
                   vps_client_statement_metadata(statement, &metadata,
                                                 &fixture.error) ==
                       VPS_CLIENT_OK &&
                   metadata.published_row_count == 2U &&
                   metadata.affected_count_valid &&
                   metadata.affected_count == 2U &&
                   fixture.fake.single_row_count == 1U &&
                   fixture.fake.clear_count == 3U,
               "stream_terminal");
    TEST_CHECK(vps_client_statement_close(&statement) == VPS_CLIENT_OK &&
                   vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "stream_cleanup");

    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "late_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.sockets[0] = 44;
    fixture.fake.poll_length = 1U;
    fixture.fake.described_field_count = 1;
    fixture.fake.described_field_types[0] = 23U;
    fixture.fake.stream_statuses[0] = VPS_LIBPQ_RESULT_SINGLE_TUPLE;
    fixture.fake.stream_statuses[1] = VPS_LIBPQ_RESULT_FATAL_ERROR;
    fixture.fake.stream_length = 2U;
    fixture.fake.result_sqlstate = "22012";
    fixture.fake.result_primary_message =
        "division by zero near \"synthetic-secret\"";
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_open(connection, &spec, &statement,
                                             &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_EXECUTE,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_FETCH,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_OK &&
                   vps_client_statement_row_consumed(
                       statement, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_FETCH,
                       &fixture.error) == VPS_CLIENT_OK,
               "late_start");
    TEST_CHECK(test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_FAILED &&
                   fixture.error.sqlstate[0] == '2' &&
                   fixture.fake.clear_count == 2U &&
                   fixture.primary_message_seen &&
                   !fixture.forbidden_seen,
               "late_propagated");
#if defined(VPS_DEBUG)
    TEST_CHECK(fixture.sql_text_seen, "late_debug_sql");
#else
    TEST_CHECK(!fixture.sql_text_seen, "late_release_no_sql");
#endif
    TEST_CHECK(vps_client_statement_close(&statement) == VPS_CLIENT_OK &&
                   vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "late_cleanup");
    return 1;
}

static int test_success_and_socket_replacement(void)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL),
               "success_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_READING;
    fixture.fake.polls[1] = VPS_LIBPQ_POLL_WRITING;
    fixture.fake.polls[2] = VPS_LIBPQ_POLL_ACTIVE;
    fixture.fake.polls[3] = VPS_LIBPQ_POLL_OK;
    fixture.fake.sockets[0] = 11;
    fixture.fake.sockets[1] = 22;
    fixture.fake.poll_length = 4U;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK,
               "success_start");
    TEST_CHECK(test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_READY,
               "success_drive");
    TEST_CHECK(fixture.fake.keyword_view_valid &&
                   fixture.fake.start_count == 1U &&
                   fixture.fake.nonblocking_count == 1U &&
                   fixture.fake.poll_count == 4U &&
                   platform.wait_count == 2U &&
                   platform.observed_socket[0] == 11 &&
                   platform.observed_socket[1] == 22 &&
                   platform.observed_interest[0] == VPS_WAIT_READ &&
                   platform.observed_interest[1] == VPS_WAIT_WRITE,
               "success_sequence");
    TEST_CHECK(fixture.fake.readiness_count == 2U &&
                   fixture.fake.readiness_order[0] == 1 &&
                   fixture.fake.readiness_order[1] == 2,
               "success_readiness_order");
    TEST_CHECK(fixture.log_count > 0U && !fixture.forbidden_seen,
               "success_redaction");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK,
               "success_close");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   fixture.fake.finish_count == 1U &&
                   test_fixture_cleanup(&fixture),
               "success_cleanup");
    return 1;
}

static int test_poll_failure_class(VpsErrorClass error_class,
                                   const char *name)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), name);
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_FAILED;
    fixture.fake.poll_length = 1U;
    fixture.fake.error_class = error_class;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   fixture.error.error_class == error_class,
               name);
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   fixture.fake.finish_count == 1U &&
                   test_fixture_cleanup(&fixture),
               "poll_failure_cleanup");
    return 1;
}

static int test_timeout_and_interrupt(void)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL),
               "timeout_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_READING;
    fixture.fake.sockets[0] = 31;
    fixture.fake.poll_length = 1U;
    platform.wait_status = VPS_PLATFORM_TIMEOUT;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   fixture.error.error_class == VPS_ERROR_CLASS_TIMEOUT &&
                   platform.wait_count == 10U,
               "timeout_wait");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "timeout_cleanup");
    return 1;
}

static VpsInterruptProbeResult fake_interrupt(void *context)
{
    return context != NULL ? VPS_INTERRUPT_REQUESTED
                           : VPS_INTERRUPT_CONTINUE;
}

static int test_interrupt(void)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL),
               "interrupt_fixture");
    fixture.adapter.interrupt_probe = fake_interrupt;
    fixture.adapter.interrupt_context = &fixture;
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_WRITING;
    fixture.fake.sockets[0] = 41;
    fixture.fake.poll_length = 1U;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   fixture.error.error_class == VPS_ERROR_CLASS_CANCEL &&
                   platform.wait_count == 0U,
               "interrupt_wait");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "interrupt_cleanup");
    return 1;
}

static int test_partial_and_fault_cleanup(void)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    VpsAllocator system_allocator;
    VpsAllocator fault_allocator;
    VpsFaultAllocator fault;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL),
               "partial_fixture");
    fixture.fake.nonblocking_result = 1;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_BACKEND_ERROR &&
                   fixture.fake.finish_count == 0U,
               "partial_start");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   fixture.fake.finish_count == 1U &&
                   test_fixture_cleanup(&fixture),
               "partial_cleanup");

    TEST_CHECK(vps_allocator_system(&system_allocator) == VPS_MEMORY_OK &&
                   vps_fault_allocator_init(&fault, &system_allocator, 1U) ==
                       VPS_MEMORY_OK &&
                   vps_fault_allocator_make(&fault, &fault_allocator) ==
                       VPS_MEMORY_OK &&
                   test_fixture_init(&fixture, &platform,
                                     &platform_operations, &fault_allocator),
               "fault_fixture");
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) ==
                   VPS_CLIENT_OUT_OF_MEMORY &&
                   connection == NULL && fixture.fake.start_count == 0U &&
                   fault.active_allocations == 0U &&
                   test_fixture_cleanup(&fixture),
               "fault_backend_allocation");
    return 1;
}

static int test_invalid_socket_and_readiness(void)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL),
               "invalid_socket_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_READING;
    fixture.fake.sockets[0] = -1;
    fixture.fake.poll_length = 1U;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   fixture.error.error_class == VPS_ERROR_CLASS_CONNECTION,
               "invalid_socket");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "invalid_socket_cleanup");

    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL),
               "tls_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.poll_length = 1U;
    fixture.fake.tls_result = VPS_TLS_REQUIRED;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   fixture.error.error_class == VPS_ERROR_CLASS_TLS &&
                   fixture.fake.identity_count == 0U,
               "tls_fail_closed");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "tls_cleanup");

    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL),
               "identity_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.poll_length = 1U;
    fixture.fake.identity_result = 0;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   fixture.error.error_class == VPS_ERROR_CLASS_CONFIG,
               "identity_fail_closed");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "identity_cleanup");

    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL),
               "session_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.poll_length = 1U;
    fixture.session_plan.setting_count = 1U;
    fixture.session_plan.storage_size = 4U;
    (void)memcpy(fixture.session_plan.storage, "UTF8", 4U);
    fixture.session_plan.settings[0].parameter =
        VPS_SESSION_PARAMETER_CLIENT_ENCODING;
    fixture.session_plan.settings[0].expected_class =
        VPS_SESSION_EXPECTED_EXACT;
    fixture.session_plan.settings[0].value_offset = 0U;
    fixture.session_plan.settings[0].value_length = 4U;
    fixture.fake.health_mismatch = 1;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   fixture.error.error_class == VPS_ERROR_CLASS_CONFIG,
               "session_fail_closed");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "session_cleanup");
    return 1;
}

static int test_ping_reset_health(void)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "health_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.sockets[0] = 48;
    fixture.fake.poll_length = 1U;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK,
               "health_connect");
    TEST_CHECK(vps_client_connection_start(
                   connection, VPS_CLIENT_OPERATION_PING,
                   &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_READY,
               "health_ping");
    TEST_CHECK(vps_client_connection_start(
                   connection, VPS_CLIENT_OPERATION_RESET,
                   &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_READY &&
                   fixture.fake.clear_count == 2U,
               "health_reset");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "health_cleanup");
    return 1;
}

static int test_connection_transactions(void)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "transaction_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.poll_length = 1U;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK,
               "transaction_connect");
    TEST_CHECK(vps_client_connection_start(
                   connection, VPS_CLIENT_OPERATION_BEGIN,
                   &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_TRANSACTION_ACTIVE &&
                   fixture.fake.transaction_status ==
                       VPS_LIBPQ_TRANSACTION_ACTIVE,
               "transaction_begin");
    TEST_CHECK(vps_client_connection_start(
                   connection, VPS_CLIENT_OPERATION_COMMIT,
                   &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_READY &&
                   fixture.fake.transaction_status ==
                       VPS_LIBPQ_TRANSACTION_IDLE,
               "transaction_commit");
    TEST_CHECK(vps_client_connection_start(
                   connection, VPS_CLIENT_OPERATION_BEGIN,
                   &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK,
               "transaction_second_begin");
    fixture.fake.transaction_status = VPS_LIBPQ_TRANSACTION_INERROR;
    TEST_CHECK(vps_client_connection_start(
                   connection, VPS_CLIENT_OPERATION_ROLLBACK,
                   &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   fixture.fake.transaction_status ==
                       VPS_LIBPQ_TRANSACTION_IDLE,
               "transaction_aborted_rollback");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "transaction_cleanup");
    return 1;
}

static int test_health_dirty_rejection(void)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;

    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "transaction_dirty_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.poll_length = 1U;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK,
               "transaction_dirty_connect");
    fixture.fake.transaction_status = VPS_LIBPQ_TRANSACTION_ACTIVE;
    TEST_CHECK(vps_client_connection_start(
                   connection, VPS_CLIENT_OPERATION_PING,
                   &fixture.error) == VPS_CLIENT_BACKEND_ERROR &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_FAILED,
               "transaction_dirty_rejected");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "transaction_dirty_cleanup");

    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "pipeline_dirty_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.poll_length = 1U;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK,
               "pipeline_dirty_connect");
    fixture.fake.pipeline_status = VPS_LIBPQ_PIPELINE_ON;
    TEST_CHECK(vps_client_connection_start(
                   connection, VPS_CLIENT_OPERATION_RESET,
                   &fixture.error) == VPS_CLIENT_BACKEND_ERROR,
               "pipeline_dirty_rejected");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "pipeline_dirty_cleanup");

    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "pending_dirty_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.poll_length = 1U;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK,
               "pending_dirty_connect");
    fixture.fake.current_command = 6;
    fixture.fake.get_result_step = 0;
    TEST_CHECK(vps_client_connection_start(
                   connection, VPS_CLIENT_OPERATION_PING,
                   &fixture.error) == VPS_CLIENT_BACKEND_ERROR &&
                   fixture.fake.clear_count == 1U,
               "pending_dirty_rejected");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "pending_dirty_cleanup");
    return 1;
}

static int test_strict_reset_and_reset_failure(void)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "strict_reset_fixture");
    fixture.adapter.reset_mode = VPS_LIBPQ_RESET_STRICT;
    fixture.session_plan.setting_count = 1U;
    fixture.session_plan.storage_size = 4U;
    (void)memcpy(fixture.session_plan.storage, "UTF8", 4U);
    fixture.session_plan.settings[0].parameter =
        VPS_SESSION_PARAMETER_CLIENT_ENCODING;
    fixture.session_plan.settings[0].expected_class =
        VPS_SESSION_EXPECTED_EXACT;
    fixture.session_plan.settings[0].value_length = 4U;
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.poll_length = 1U;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_RESET,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   fixture.fake.clear_count == 2U,
               "strict_reset_applied");
    fixture.fake.send_result = 0;
    TEST_CHECK(vps_client_connection_start(
                   connection, VPS_CLIENT_OPERATION_RESET,
                   &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_FAILED,
               "reset_failure_dirty");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "strict_reset_cleanup");
    return 1;
}

static int test_secure_cancel_and_failure(void)
{
    static const char query[] = "SELECT pg_catalog.generate_series(1, 2)";
    VpsClientResultFieldExpectation field;
    VpsClientStatementSpec spec;
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    VpsClientStatement *statement = NULL;

    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "cancel_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.poll_length = 1U;
    fixture.fake.described_field_count = 1;
    fixture.fake.described_field_types[0] = 23U;
    fixture.fake.stream_statuses[0] = VPS_LIBPQ_RESULT_SINGLE_TUPLE;
    fixture.fake.stream_statuses[1] = VPS_LIBPQ_RESULT_FATAL_ERROR;
    fixture.fake.stream_length = 2U;
    fixture.fake.result_sqlstate = "57014";
    fixture.fake.cancel_polls[0] = VPS_LIBPQ_POLL_READING;
    fixture.fake.cancel_polls[1] = VPS_LIBPQ_POLL_OK;
    fixture.fake.cancel_poll_length = 2U;
    field.type_oid = 23U;
    field.format = VPS_CLIENT_VALUE_TEXT;
    (void)memset(&spec, 0, sizeof(spec));
    spec.query = query;
    spec.query_length = sizeof(query) - 1U;
    spec.result_fields = &field;
    spec.result_field_count = 1U;
    spec.timeout_ms = 100U;
    spec.single_row = 1;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_open(connection, &spec, &statement,
                                             &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_EXECUTE,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_FETCH,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_OK &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_ROW_READY,
               "cancel_after_row");
    fixture.adapter.interrupt_probe = fake_interrupt;
    fixture.adapter.interrupt_context = &fixture;
    TEST_CHECK(vps_client_statement_start(
                   statement, VPS_CLIENT_OPERATION_CANCEL,
                   &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_CANCEL,
                       &fixture.error) == VPS_CLIENT_INVALID_STATE &&
                   test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_OK &&
                   vps_client_statement_state(statement) ==
                       VPS_CLIENT_STATEMENT_COMPLETE &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_READY &&
                   fixture.error.error_class == VPS_ERROR_CLASS_CANCEL &&
                   strcmp(fixture.error.sqlstate, "57014") == 0 &&
                   fixture.fake.cancel_create_count == 1U &&
                   fixture.fake.cancel_start_count == 1U &&
                   fixture.fake.cancel_finish_count == 1U &&
                   fixture.fake.clear_count == 2U &&
                   platform.observed_socket[0] == fixture.fake.cancel_socket,
               "cancel_drained");
    TEST_CHECK(vps_client_statement_close(&statement) == VPS_CLIENT_OK &&
                   vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "cancel_cleanup");

    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL), "cancel_failure_fixture");
    fixture.fake.polls[0] = VPS_LIBPQ_POLL_OK;
    fixture.fake.poll_length = 1U;
    fixture.fake.cancel_polls[0] = VPS_LIBPQ_POLL_FAILED;
    fixture.fake.cancel_poll_length = 1U;
    spec.single_row = 0;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive(connection, &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_open(connection, &spec, &statement,
                                             &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_EXECUTE,
                       &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_statement_start(
                       statement, VPS_CLIENT_OPERATION_CANCEL,
                       &fixture.error) == VPS_CLIENT_OK &&
                   test_drive_statement(statement, &fixture.error) ==
                       VPS_CLIENT_BACKEND_ERROR &&
                   fixture.fake.cancel_finish_count == 1U,
               "cancel_failure_destroys");
    TEST_CHECK(vps_client_statement_close(&statement) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_PING,
                       &fixture.error) == VPS_CLIENT_INVALID_STATE &&
                   vps_client_connection_state(connection) ==
                       VPS_CLIENT_CONNECTION_FAILED &&
                   vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   test_fixture_cleanup(&fixture),
               "cancel_failure_cleanup");
    return 1;
}

static int test_api_validation_and_null_start(void)
{
    TestFixture fixture;
    FakePlatform platform;
    VpsPlatformOperations platform_operations;
    VpsClientConnection *connection = NULL;
    VpsLibpqClientOptions options;
    VpsLibpqClient adapter;
    TEST_CHECK(test_fixture_init(&fixture, &platform, &platform_operations,
                                 NULL),
               "null_start_fixture");
    fixture.fake.return_null_connection = 1;
    TEST_CHECK(vps_client_connection_open(&fixture.client, &connection,
                                          &fixture.error) == VPS_CLIENT_OK &&
                   vps_client_connection_start(
                       connection, VPS_CLIENT_OPERATION_CONNECT,
                       &fixture.error) == VPS_CLIENT_OUT_OF_MEMORY &&
                   fixture.error.error_class == VPS_ERROR_CLASS_MEMORY,
               "null_start");
    TEST_CHECK(vps_client_connection_close(&connection) == VPS_CLIENT_OK &&
                   fixture.fake.finish_count == 0U &&
                   test_fixture_cleanup(&fixture),
               "null_start_cleanup");
    (void)memset(&options, 0, sizeof(options));
    options.allocator = &fixture.allocator;
    options.platform_operations = &platform_operations;
    options.connection_config = &fixture.config;
    options.identity = &fixture.identity;
    options.tls_policy = &fixture.tls_policy;
    options.session_plan = &fixture.session_plan;
    options.connect_timeout_ms = 100U;
    options.wait_slice_ms = 10U;
    options.api = &fixture.api;
    fixture.api.api_version += 1U;
    TEST_CHECK(vps_libpq_client_init(&adapter, &options) ==
                   VPS_CLIENT_INVALID_ARGUMENT,
               "api_version");
    fixture.api = fake_api(&fixture.fake);
    fixture.api.connect_poll = NULL;
    TEST_CHECK(vps_libpq_client_init(&adapter, &options) ==
                   VPS_CLIENT_INVALID_ARGUMENT,
               "api_missing_callback");
    fixture.api = fake_api(&fixture.fake);
    fixture.config.config.header.present_fields |=
        VPS_CREDENTIAL_FIELD_SSLROOTCERT;
    fixture.config.config.sslrootcert = NULL;
    TEST_CHECK(vps_libpq_client_init(&adapter, &options) ==
                   VPS_CLIENT_INVALID_ARGUMENT,
               "api_invalid_config");
    return 1;
}

int main(void)
{
    if (!test_statement_prepare_describe_execute() ||
        !test_statement_metadata_mismatch() ||
        !test_single_row_and_late_error() ||
        !test_success_and_socket_replacement() ||
        !test_poll_failure_class(VPS_ERROR_CLASS_AUTH, "auth_failure") ||
        !test_poll_failure_class(VPS_ERROR_CLASS_CONNECTION, "refused") ||
        !test_timeout_and_interrupt() || !test_interrupt() ||
        !test_partial_and_fault_cleanup() ||
        !test_invalid_socket_and_readiness() ||
        !test_ping_reset_health() ||
        !test_connection_transactions() ||
        !test_health_dirty_rejection() ||
        !test_strict_reset_and_reset_failure() ||
        !test_secure_cancel_and_failure() ||
        !test_api_validation_and_null_start()) {
        return 1;
    }
    (void)fprintf(stdout, "vps_libpq_client_test status=passed\n");
    return 0;
}
