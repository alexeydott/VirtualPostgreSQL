#include "vps_session.h"

#include <stdio.h>
#include <string.h>

#define TEST_CHECK(condition, name)                                  \
    do {                                                             \
        if (!(condition)) {                                          \
            (void)fprintf(stderr, "session_case=%s status=failed\n", \
                          (name));                                   \
            return 0;                                                \
        }                                                            \
    } while (0)

typedef struct TestFixture {
    VpsAllocator allocator;
    VpsParsedArguments arguments;
    VpsConnectionConfig connection;
} TestFixture;

typedef struct TestClient {
    size_t apply_count;
    size_t inspect_count;
    size_t fail_at;
    size_t dirty_at_inspect;
    int dirty_kind;
    size_t inspect_error_at;
    int order_valid;
} TestClient;

typedef struct TestLogCapture {
    size_t parameter_fields;
    size_t expected_fields;
    int value_seen;
} TestLogCapture;

static int test_log_sink(void *context, const VpsLogEvent *event)
{
    static const char forbidden[] = "Tenant Schema";
    TestLogCapture *capture = (TestLogCapture *)context;
    size_t index;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->key == VPS_LOG_FIELD_PARAMETER) {
            capture->parameter_fields += 1U;
        } else if (field->key == VPS_LOG_FIELD_EXPECTED_CLASS) {
            capture->expected_fields += 1U;
        }
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            field->value.string_value.length == sizeof(forbidden) - 1U &&
            memcmp(field->value.string_value.data, forbidden,
                   sizeof(forbidden) - 1U) == 0) {
            capture->value_seen = 1;
        }
    }
    return 0;
}

static int test_fixture_init(TestFixture *fixture,
                             const VpsArgumentInput *inputs,
                             size_t input_count,
                             VpsLogger *logger)
{
    (void)memset(fixture, 0, sizeof(*fixture));
    return vps_allocator_system(&fixture->allocator) == VPS_MEMORY_OK &&
           vps_arguments_init(&fixture->arguments, &fixture->allocator,
                              vps_platform_current_operations(), logger) ==
               VPS_ARGUMENTS_OK &&
           vps_arguments_parse(&fixture->arguments, inputs, input_count,
                               NULL) == VPS_ARGUMENTS_OK &&
           vps_connection_config_init(&fixture->connection,
                                      &fixture->allocator,
                                      vps_platform_current_operations(),
                                      logger) == VPS_CONNECTION_STRING_OK;
}

static void test_fixture_cleanup(TestFixture *fixture)
{
    (void)vps_connection_config_cleanup(&fixture->connection);
    (void)vps_arguments_reset(&fixture->arguments);
}

static const VpsSessionSetting *test_setting(const VpsSessionPlan *plan,
                                             VpsSessionParameter parameter)
{
    size_t index;
    for (index = 0U; index < plan->setting_count; ++index) {
        const VpsSessionSetting *setting = vps_session_setting_at(plan, index);
        if (setting != NULL && setting->parameter == parameter) return setting;
    }
    return NULL;
}

static int test_setting_equals(const VpsSessionPlan *plan,
                               VpsSessionParameter parameter,
                               const char *value)
{
    const VpsSessionSetting *setting = test_setting(plan, parameter);
    size_t length = strlen(value);
    const char *actual = vps_session_setting_value(plan, setting);
    return setting != NULL && actual != NULL &&
           setting->value_length == length &&
           memcmp(actual, value, length) == 0;
}

static VpsSessionResult test_client_inspect(void *context,
                                            VpsSessionConnectionState *state)
{
    TestClient *client = (TestClient *)context;
    client->inspect_count += 1U;
    state->transaction_idle = 1;
    state->pipeline_disabled = 1;
    state->pending_results_absent = 1;
    if (client->inspect_count == client->inspect_error_at) {
        return VPS_SESSION_CLIENT_ERROR;
    }
    if (client->inspect_count == client->dirty_at_inspect) {
        if (client->dirty_kind == 1) state->transaction_idle = 0;
        else if (client->dirty_kind == 2) state->pipeline_disabled = 0;
        else state->pending_results_absent = 0;
    }
    return VPS_SESSION_OK;
}

static VpsSessionResult test_client_apply(void *context,
                                          const VpsSessionPlan *plan,
                                          const VpsSessionSetting *setting)
{
    TestClient *client = (TestClient *)context;
    const char *value = vps_session_setting_value(plan, setting);
    if (setting == NULL ||
        setting->parameter != (VpsSessionParameter)client->apply_count ||
        value == NULL || !vps_session_setting_matches(
            plan, setting, value, setting->value_length)) {
        client->order_valid = 0;
        return VPS_SESSION_CLIENT_ERROR;
    }
    if (client->apply_count == client->fail_at) {
        client->apply_count += 1U;
        return VPS_SESSION_CLIENT_ERROR;
    }
    client->apply_count += 1U;
    return VPS_SESSION_OK;
}

static int test_build_and_matching(void)
{
    static const VpsArgumentInput inputs[] = {
        {"service=test", 12U},
        {"source=table", 12U},
        {"schema=public", 13U},
        {"table=fixture", 13U},
        {"mode=ro", 7U},
        {"statement_timeout=1500", 22U},
        {"lock_timeout=250", 16U}};
    TestFixture fixture;
    VpsSessionPlan plan;
    VpsSessionBuildOptions options = {NULL, 0U, 2000U};
    const VpsSessionSetting *timeout;
    TEST_CHECK(test_fixture_init(&fixture, inputs,
                                 sizeof(inputs) / sizeof(inputs[0]), NULL),
               "fixture_init");
    fixture.connection.config.statement_timeout = "999";
    fixture.connection.config.lock_timeout = "999";
    TEST_CHECK(vps_session_plan_init(&plan, NULL) == VPS_SESSION_OK &&
                   vps_session_plan_build(&plan, &fixture.connection,
                                          &fixture.arguments, &options) ==
                       VPS_SESSION_OK &&
                   plan.setting_count == VPS_SESSION_SETTING_COUNT,
               "build_defaults");
    TEST_CHECK(test_setting_equals(&plan, VPS_SESSION_PARAMETER_CLIENT_ENCODING,
                                   "UTF8") &&
                   test_setting_equals(&plan, VPS_SESSION_PARAMETER_DATESTYLE,
                                       "ISO, YMD") &&
                   test_setting_equals(&plan, VPS_SESSION_PARAMETER_INTERVALSTYLE,
                                       "iso_8601") &&
                   test_setting_equals(&plan, VPS_SESSION_PARAMETER_TIMEZONE,
                                       "UTC") &&
                   test_setting_equals(&plan,
                                       VPS_SESSION_PARAMETER_STANDARD_STRINGS,
                                       "on") &&
                   test_setting_equals(&plan,
                                       VPS_SESSION_PARAMETER_APPLICATION_NAME,
                                       VPS_SESSION_DEFAULT_APPLICATION_NAME) &&
                   test_setting_equals(&plan, VPS_SESSION_PARAMETER_SEARCH_PATH,
                                       "pg_catalog") &&
                   test_setting_equals(&plan,
                                       VPS_SESSION_PARAMETER_STATEMENT_TIMEOUT,
                                       "1500") &&
                   test_setting_equals(&plan, VPS_SESSION_PARAMETER_LOCK_TIMEOUT,
                                       "250") &&
                   test_setting_equals(
                       &plan, VPS_SESSION_PARAMETER_IDLE_TRANSACTION_TIMEOUT,
                       "2000") &&
                   test_setting_equals(&plan,
                                       VPS_SESSION_PARAMETER_DEFAULT_READ_ONLY,
                                       "on"),
               "baseline_values");
    timeout = test_setting(&plan, VPS_SESSION_PARAMETER_STATEMENT_TIMEOUT);
    TEST_CHECK(vps_session_setting_matches(&plan, timeout, "1500ms", 6U) &&
                   !vps_session_setting_matches(&plan, timeout, "2s", 2U),
               "timeout_normalization");
    vps_session_plan_reset(&plan);
    TEST_CHECK(plan.initialized && !plan.built && plan.setting_count == 0U &&
                   plan.storage_size == 0U,
               "reset_idempotent");
    vps_session_plan_reset(&plan);
    test_fixture_cleanup(&fixture);
    return 1;
}

static int test_validation_and_transactional_build(void)
{
    static const VpsArgumentInput inputs[] = {
        {"service=test", 12U}, {"source=table", 12U},
        {"schema=public", 13U}, {"table=fixture", 13U},
        {"mode=rw", 7U}};
    static const char valid_path[] = "\"Tenant Schema\", pg_catalog";
    static const char hostile_path[] = "pg_catalog; DROP SCHEMA tenant";
    static const char overflow_timeout[] = "4294967296";
    TestFixture fixture;
    VpsSessionPlan plan;
    VpsSessionPlan published;
    VpsSessionBuildOptions options = {"Europe/Moscow", 13U, UINT32_MAX};
    const VpsSessionSetting *idle_timeout;
    TEST_CHECK(test_fixture_init(&fixture, inputs,
                                 sizeof(inputs) / sizeof(inputs[0]), NULL),
               "validation_init");
    fixture.connection.config.search_path = valid_path;
    fixture.connection.config.application_name = "VirtualPostgreSQL/test";
    fixture.connection.config.statement_timeout = "1";
    TEST_CHECK(vps_session_plan_init(&plan, NULL) == VPS_SESSION_OK &&
                   vps_session_plan_build(&plan, &fixture.connection,
                                          &fixture.arguments, &options) ==
                       VPS_SESSION_OK &&
                   test_setting_equals(&plan, VPS_SESSION_PARAMETER_TIMEZONE,
                                       "Europe/Moscow") &&
                   test_setting_equals(&plan,
                                       VPS_SESSION_PARAMETER_DEFAULT_READ_ONLY,
                                       "off") &&
                   test_setting_equals(
                       &plan, VPS_SESSION_PARAMETER_IDLE_TRANSACTION_TIMEOUT,
                       "4294967295"),
               "valid_custom_values");
    idle_timeout = test_setting(
        &plan, VPS_SESSION_PARAMETER_IDLE_TRANSACTION_TIMEOUT);
    TEST_CHECK(vps_session_setting_matches(
                   &plan, idle_timeout, "4294967295ms", 12U) &&
                   !vps_session_setting_matches(
                       &plan, idle_timeout, "4294967296ms", 12U),
               "timeout_uint32_boundary");
    published = plan;
    fixture.connection.config.search_path = hostile_path;
    TEST_CHECK(vps_session_plan_build(&plan, &fixture.connection,
                                      &fixture.arguments, &options) ==
                       VPS_SESSION_INVALID_VALUE &&
                   memcmp(&plan, &published, sizeof(plan)) == 0,
               "hostile_search_path");
    fixture.connection.config.search_path = valid_path;
    fixture.connection.config.statement_timeout = overflow_timeout;
    TEST_CHECK(vps_session_plan_build(&plan, &fixture.connection,
                                      &fixture.arguments, &options) ==
                       VPS_SESSION_INVALID_VALUE &&
                   memcmp(&plan, &published, sizeof(plan)) == 0,
               "timeout_overflow");
    fixture.connection.config.statement_timeout = "1";
    fixture.connection.config.search_path = "\"unterminated, pg_catalog";
    TEST_CHECK(vps_session_plan_build(&plan, &fixture.connection,
                                      &fixture.arguments, &options) ==
                       VPS_SESSION_INVALID_VALUE &&
                   memcmp(&plan, &published, sizeof(plan)) == 0,
               "malformed_quoted_search_path");
    fixture.connection.config.search_path = valid_path;
    options.timezone = "UTC\nunsafe";
    options.timezone_length = 10U;
    TEST_CHECK(vps_session_plan_build(&plan, &fixture.connection,
                                      &fixture.arguments, &options) ==
                       VPS_SESSION_INVALID_VALUE &&
                   memcmp(&plan, &published, sizeof(plan)) == 0,
               "timezone_control_rejected");
    TEST_CHECK(vps_session_plan_build(&plan, &fixture.connection,
                                      &fixture.arguments, NULL) ==
                       VPS_SESSION_INVALID_ARGUMENT &&
                   memcmp(&plan, &published, sizeof(plan)) == 0,
               "invalid_build_is_transactional");
    test_fixture_cleanup(&fixture);
    return 1;
}

static int test_apply_order_failures_and_logging(void)
{
    static const VpsArgumentInput inputs[] = {
        {"service=test", 12U}, {"source=table", 12U},
        {"schema=public", 13U}, {"table=fixture", 13U},
        {"mode=ro", 7U}};
    TestFixture fixture;
    VpsSessionPlan plan;
    VpsSessionBuildOptions options = {NULL, 0U, 0U};
    VpsSessionClientOperations operations;
    TestClient client;
    TestLogCapture capture = {0};
    VpsLogger logger;
    TEST_CHECK(vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG, test_log_sink,
                               &capture) == VPS_LOG_OK &&
                   test_fixture_init(&fixture, inputs,
                                     sizeof(inputs) / sizeof(inputs[0]),
                                     &logger),
               "apply_init");
    fixture.connection.config.search_path = "\"Tenant Schema\", pg_catalog";
    TEST_CHECK(vps_session_plan_init(&plan, &logger) == VPS_SESSION_OK &&
                   vps_session_plan_build(&plan, &fixture.connection,
                                          &fixture.arguments, &options) ==
                       VPS_SESSION_OK,
               "apply_build");
    (void)memset(&client, 0, sizeof(client));
    client.fail_at = (size_t)-1;
    client.dirty_at_inspect = (size_t)-1;
    client.inspect_error_at = (size_t)-1;
    client.order_valid = 1;
    operations.context = &client;
    operations.apply_setting = test_client_apply;
    operations.inspect = test_client_inspect;
    TEST_CHECK(vps_session_apply(&plan, &operations,
                                 VPS_SESSION_PHASE_CONNECT) == VPS_SESSION_OK &&
                   client.apply_count == VPS_SESSION_SETTING_COUNT &&
                   client.inspect_count == 2U && client.order_valid,
               "ordered_apply");
    (void)memset(&client, 0, sizeof(client));
    client.fail_at = 4U;
    client.dirty_at_inspect = (size_t)-1;
    client.inspect_error_at = (size_t)-1;
    client.order_valid = 1;
    operations.context = &client;
    TEST_CHECK(vps_session_apply(&plan, &operations, VPS_SESSION_PHASE_RESET) ==
                       VPS_SESSION_CLIENT_ERROR &&
                   client.apply_count == 5U && client.inspect_count == 1U &&
                   client.order_valid,
               "partial_failure_stops");
    (void)memset(&client, 0, sizeof(client));
    client.fail_at = (size_t)-1;
    client.dirty_at_inspect = 1U;
    client.inspect_error_at = (size_t)-1;
    client.order_valid = 1;
    operations.context = &client;
    TEST_CHECK(vps_session_apply(&plan, &operations, VPS_SESSION_PHASE_RESET) ==
                       VPS_SESSION_CONNECTION_DIRTY &&
                   client.apply_count == 0U && client.inspect_count == 1U,
               "dirty_before_apply");
    (void)memset(&client, 0, sizeof(client));
    client.fail_at = (size_t)-1;
    client.dirty_at_inspect = 1U;
    client.dirty_kind = 1;
    client.inspect_error_at = (size_t)-1;
    client.order_valid = 1;
    operations.context = &client;
    TEST_CHECK(vps_session_apply(&plan, &operations, VPS_SESSION_PHASE_RESET) ==
                       VPS_SESSION_CONNECTION_DIRTY &&
                   client.apply_count == 0U && client.inspect_count == 1U,
               "transaction_not_idle");
    (void)memset(&client, 0, sizeof(client));
    client.fail_at = (size_t)-1;
    client.dirty_at_inspect = 1U;
    client.dirty_kind = 2;
    client.inspect_error_at = (size_t)-1;
    client.order_valid = 1;
    operations.context = &client;
    TEST_CHECK(vps_session_apply(&plan, &operations, VPS_SESSION_PHASE_RESET) ==
                       VPS_SESSION_CONNECTION_DIRTY &&
                   client.apply_count == 0U && client.inspect_count == 1U,
               "pipeline_not_disabled");
    (void)memset(&client, 0, sizeof(client));
    client.fail_at = (size_t)-1;
    client.dirty_at_inspect = 2U;
    client.inspect_error_at = (size_t)-1;
    client.order_valid = 1;
    operations.context = &client;
    TEST_CHECK(vps_session_apply(&plan, &operations, VPS_SESSION_PHASE_RESET) ==
                       VPS_SESSION_CONNECTION_DIRTY &&
                   client.apply_count == VPS_SESSION_SETTING_COUNT &&
                   client.inspect_count == 2U,
               "dirty_after_apply");
    (void)memset(&client, 0, sizeof(client));
    client.fail_at = (size_t)-1;
    client.dirty_at_inspect = (size_t)-1;
    client.inspect_error_at = 1U;
    client.order_valid = 1;
    operations.context = &client;
    TEST_CHECK(vps_session_apply(&plan, &operations, VPS_SESSION_PHASE_RESET) ==
                       VPS_SESSION_CLIENT_ERROR &&
                   client.apply_count == 0U && client.inspect_count == 1U,
               "inspect_failure_propagated");
    TEST_CHECK(capture.parameter_fields > 0U &&
                   capture.parameter_fields == capture.expected_fields &&
                   !capture.value_seen,
               "redacted_logging");
    test_fixture_cleanup(&fixture);
    return 1;
}

int main(void)
{
    if (!test_build_and_matching() ||
        !test_validation_and_transactional_build() ||
        !test_apply_order_failures_and_logging()) return 1;
    (void)printf("session_tests=passed\n");
    return 0;
}
