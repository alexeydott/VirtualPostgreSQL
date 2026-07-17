#include "vps_connection_string.h"
#include "vps_libpq_client_conninfo.h"

#include <stdio.h>
#include <string.h>

#define TEST_CHECK(condition, name)                                      \
    do {                                                                 \
        if (!(condition)) {                                              \
            (void)fprintf(stderr, "connection_case=%s status=failed\n", \
                          (name));                                       \
            return 0;                                                    \
        }                                                                \
    } while (0)

#define INPUT_LITERAL(value) {value, sizeof(value) - 1U}

typedef struct TestLogCapture {
    size_t events;
    int secret_seen;
    int profile_seen;
    int service_seen;
} TestLogCapture;

static VpsConnectionStringResult reject_test_consumer(
    void *context,
    const VpsCredentialConfig *config);

static int test_log_sink(void *context, const VpsLogEvent *event)
{
    static const char secret[] = "opaque-value-28";
    TestLogCapture *capture = (TestLogCapture *)context;
    size_t index;
    capture->events += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->key == VPS_LOG_FIELD_PROFILE) capture->profile_seen = 1;
        if (field->key == VPS_LOG_FIELD_SERVICE) capture->service_seen = 1;
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            field->value.string_value.length == sizeof(secret) - 1U &&
            memcmp(field->value.string_value.data, secret,
                   sizeof(secret) - 1U) == 0) {
            capture->secret_seen = 1;
        }
    }
    return 0;
}

static int test_context_init(VpsAllocator *allocator,
                             VpsLogger *logger,
                             TestLogCapture *capture,
                             VpsParsedArguments *arguments,
                             VpsConnectionConfig *connection)
{
    const VpsPlatformOperations *operations = vps_platform_current_operations();
    return vps_allocator_system(allocator) == VPS_MEMORY_OK &&
           vps_logger_init(logger, VPS_LOG_LEVEL_DEBUG, test_log_sink, capture) ==
               VPS_LOG_OK &&
           vps_arguments_init(arguments, allocator, operations, logger) ==
               VPS_ARGUMENTS_OK &&
           vps_connection_config_init(connection, allocator, operations, logger) ==
               VPS_CONNECTION_STRING_OK;
}

static int parse(VpsParsedArguments *arguments,
                 const VpsArgumentInput *inputs,
                 size_t count)
{
    VpsArgumentsDiagnostic diagnostic;
    return vps_arguments_parse(arguments, inputs, count, &diagnostic) ==
           VPS_ARGUMENTS_OK;
}

static int test_service_mode(void)
{
    static const VpsArgumentInput inputs[] = {
        INPUT_LITERAL("service=reporting"),
        INPUT_LITERAL("service_file='C:\\secure\\pg_service.conf'"),
        INPUT_LITERAL("source=table"), INPUT_LITERAL("schema=public"),
        INPUT_LITERAL("table=items")};
    VpsAllocator allocator;
    VpsLogger logger;
    TestLogCapture capture = {0};
    VpsParsedArguments arguments;
    VpsConnectionConfig connection;
    VpsConnectionResolveOptions options = {0};
    TEST_CHECK(test_context_init(&allocator, &logger, &capture, &arguments,
                                 &connection), "service_init");
    TEST_CHECK(parse(&arguments, inputs, sizeof(inputs) / sizeof(inputs[0])),
               "service_parse");
    TEST_CHECK(vps_connection_config_resolve(&connection, &arguments, &options) ==
                   VPS_CONNECTION_STRING_OK,
               "service_resolve");
    TEST_CHECK(connection.mode == VPS_CONNECTION_MODE_SERVICE &&
                   strcmp(connection.config.service, "reporting") == 0 &&
                   capture.service_seen && !capture.secret_seen,
               "service_contract");
    TEST_CHECK(vps_connection_config_cleanup(&connection) ==
                   VPS_CONNECTION_STRING_OK &&
                   vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "service_cleanup");
    return 1;
}

static int test_service_rejections(void)
{
    static const VpsArgumentInput traversal[] = {
        INPUT_LITERAL("service=reporting"),
        INPUT_LITERAL("service_file='C:\\secure\\..\\pg_service.conf'"),
        INPUT_LITERAL("source=table"), INPUT_LITERAL("schema=public"),
        INPUT_LITERAL("table=items")};
    VpsAllocator allocator;
    VpsLogger logger;
    TestLogCapture capture = {0};
    VpsParsedArguments arguments;
    VpsConnectionConfig connection;
    VpsConnectionResolveOptions options = {0};
    VpsArgumentMask original_presence;
    TEST_CHECK(test_context_init(&allocator, &logger, &capture, &arguments,
                                 &connection) &&
                   parse(&arguments, traversal,
                         sizeof(traversal) / sizeof(traversal[0])),
               "service_reject_init");
    TEST_CHECK(vps_connection_config_resolve(&connection, &arguments, &options) ==
                   VPS_CONNECTION_STRING_INVALID_VALUE,
               "service_traversal");
    original_presence = arguments.presence;
    arguments.presence |= VPS_ARGUMENT_PROFILE;
    TEST_CHECK(vps_connection_config_resolve(&connection, &arguments, &options) ==
                   VPS_CONNECTION_STRING_INVALID_MODE,
               "mode_boundary");
    arguments.presence = original_presence;
    TEST_CHECK(vps_connection_config_cleanup(&connection) ==
                   VPS_CONNECTION_STRING_OK &&
                   vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "service_reject_cleanup");
    return 1;
}

static int test_profile_mode(void)
{
    static const VpsArgumentInput inputs[] = {
        INPUT_LITERAL("profile=local-dev"), INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema=public"), INPUT_LITERAL("table=items")};
    static const VpsProfileEnvironmentEntry environment[] = {
        {"VPS_PROFILE_LOCAL_DEV_HOST", "localhost"},
        {"VPS_PROFILE_LOCAL_DEV_PORT", "5432"},
        {"VPS_PROFILE_LOCAL_DEV_PASSWORD", "opaque-value-28"},
        {"VPS_PROFILE_LOCAL_DEV_SSLMODE", "disable"}};
    VpsAllocator allocator;
    VpsLogger logger;
    TestLogCapture capture = {0};
    VpsParsedArguments arguments;
    VpsConnectionConfig connection;
    VpsConnectionResolveOptions options = {0};
    TEST_CHECK(test_context_init(&allocator, &logger, &capture, &arguments,
                                 &connection), "profile_init");
    TEST_CHECK(parse(&arguments, inputs, sizeof(inputs) / sizeof(inputs[0])),
               "profile_parse");
    options.profile_environment = environment;
    options.profile_environment_count =
        sizeof(environment) / sizeof(environment[0]);
    TEST_CHECK(vps_connection_config_resolve(&connection, &arguments, &options) ==
                   VPS_CONNECTION_STRING_OK,
               "profile_resolve");
    TEST_CHECK(strcmp(connection.normalized_profile, "LOCAL_DEV") == 0 &&
                   strcmp(connection.config.password, "opaque-value-28") == 0 &&
                   capture.profile_seen && !capture.secret_seen,
               "profile_contract");
    TEST_CHECK(vps_connection_config_cleanup(&connection) ==
                   VPS_CONNECTION_STRING_OK &&
                   vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "profile_cleanup");
    return 1;
}

static int test_profile_rejections(void)
{
    static const VpsArgumentInput inputs[] = {
        INPUT_LITERAL("profile=test"), INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema=public"), INPUT_LITERAL("table=items")};
    static const VpsProfileEnvironmentEntry unknown[] = {
        {"VPS_PROFILE_TEST_UNKNOWN", "value"}};
    static const VpsProfileEnvironmentEntry duplicate[] = {
        {"VPS_PROFILE_TEST_HOST", "one"},
        {"VPS_PROFILE_TEST_HOST", "two"}};
    VpsAllocator allocator;
    VpsLogger logger;
    TestLogCapture capture = {0};
    VpsParsedArguments arguments;
    VpsConnectionConfig connection;
    VpsConnectionResolveOptions options = {0};
    TEST_CHECK(test_context_init(&allocator, &logger, &capture, &arguments,
                                 &connection) &&
                   parse(&arguments, inputs, sizeof(inputs) / sizeof(inputs[0])),
               "profile_reject_init");
    options.profile_environment = unknown;
    options.profile_environment_count = 1U;
    TEST_CHECK(vps_connection_config_resolve(&connection, &arguments, &options) ==
                   VPS_CONNECTION_STRING_INVALID_VALUE,
               "profile_unknown");
    options.profile_environment = duplicate;
    options.profile_environment_count = 2U;
    TEST_CHECK(vps_connection_config_resolve(&connection, &arguments, &options) ==
                   VPS_CONNECTION_STRING_INVALID_VALUE,
               "profile_duplicate");
    TEST_CHECK(vps_connection_config_cleanup(&connection) ==
                   VPS_CONNECTION_STRING_OK &&
                   vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "profile_reject_cleanup");
    return 1;
}

static int resolve_conninfo(const char *text,
                            VpsConnectionStringResult expected,
                            int expect_password)
{
    VpsArgumentInput inputs[] = {
        {NULL, 0U}, INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema=public"), INPUT_LITERAL("table=items")};
    char argument[VPS_ARGUMENT_VALUE_LIMIT + 16U];
    VpsAllocator allocator;
    VpsLogger logger;
    TestLogCapture capture = {0};
    VpsParsedArguments arguments;
    VpsConnectionConfig connection;
    VpsConninfoParser parser = {NULL, vps_libpq_client_conninfo_parse};
    VpsConnectionResolveOptions options = {0};
    int success;
    if (strlen(text) + 9U >= sizeof(argument)) return 0;
    if (snprintf(argument, sizeof(argument), "connstr='%s'", text) < 0) return 0;
    inputs[0].text = argument;
    inputs[0].length = strlen(argument);
    if (!test_context_init(&allocator, &logger, &capture, &arguments,
                           &connection) ||
        !parse(&arguments, inputs, sizeof(inputs) / sizeof(inputs[0]))) return 0;
    options.conninfo_parser = &parser;
    success = vps_connection_config_resolve(&connection, &arguments, &options) ==
                  expected;
    if (expected == VPS_CONNECTION_STRING_OK) {
        success = success && connection.persistent_connstr_risk &&
                  ((connection.config.password != NULL) == expect_password);
    }
    success = success && !capture.secret_seen &&
              vps_connection_config_cleanup(&connection) ==
                  VPS_CONNECTION_STRING_OK &&
              vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK;
    return success;
}

static int test_conninfo_policy(void)
{
    static const char embedded_nul[] = "host=one\0user=two";
    TEST_CHECK(resolve_conninfo(
                   "host=localhost user=reader password=opaque-value-28 "
                   "sslmode=require application_name=VirtualPostgreSQL",
                   VPS_CONNECTION_STRING_OK, 1),
               "conninfo_valid");
    TEST_CHECK(resolve_conninfo("host=one host=two",
                                VPS_CONNECTION_STRING_CONNINFO_REJECTED, 0),
               "conninfo_duplicate");
    TEST_CHECK(resolve_conninfo("host=localhost options=-csearch_path=x",
                                VPS_CONNECTION_STRING_CONNINFO_REJECTED, 0),
               "conninfo_options");
    TEST_CHECK(resolve_conninfo("hostaddr=127.0.0.1",
                                VPS_CONNECTION_STRING_CONNINFO_REJECTED, 0),
               "conninfo_unknown");
    TEST_CHECK(resolve_conninfo(
                   "fallback_application_name=uncontrolled",
                   VPS_CONNECTION_STRING_CONNINFO_REJECTED, 0),
               "conninfo_fallback");
    TEST_CHECK(vps_libpq_client_conninfo_parse(
                   NULL, embedded_nul, sizeof(embedded_nul) - 1U,
                   reject_test_consumer, NULL) ==
                   VPS_CONNECTION_STRING_CONNINFO_REJECTED,
               "conninfo_embedded_nul");
    TEST_CHECK(vps_libpq_client_conninfo_parse(
                   NULL, "x", VPS_ARGUMENT_VALUE_LIMIT + 1U,
                   reject_test_consumer, NULL) ==
                   VPS_CONNECTION_STRING_CONNINFO_REJECTED,
               "conninfo_limit");
    return 1;
}

static int test_reference_without_provider(void)
{
    static const VpsArgumentInput inputs[] = {
        INPUT_LITERAL("credential_ref=app/reference"),
        INPUT_LITERAL("source=table"), INPUT_LITERAL("schema=public"),
        INPUT_LITERAL("table=items")};
    VpsAllocator allocator;
    VpsLogger logger;
    TestLogCapture capture = {0};
    VpsParsedArguments arguments;
    VpsConnectionConfig connection;
    VpsConnectionResolveOptions options = {0};
    TEST_CHECK(test_context_init(&allocator, &logger, &capture, &arguments,
                                 &connection) &&
                   parse(&arguments, inputs, sizeof(inputs) / sizeof(inputs[0])),
               "reference_init");
    TEST_CHECK(vps_connection_config_resolve(&connection, &arguments, &options) ==
                   VPS_CONNECTION_STRING_PROVIDER_UNAVAILABLE,
               "reference_absent_provider");
    TEST_CHECK(vps_connection_config_cleanup(&connection) ==
                   VPS_CONNECTION_STRING_OK &&
                   vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "reference_cleanup");
    return 1;
}

static VpsConnectionStringResult reject_test_consumer(
    void *context,
    const VpsCredentialConfig *config)
{
    (void)context;
    (void)config;
    return VPS_CONNECTION_STRING_OK;
}

int main(void)
{
    if (!test_service_mode() || !test_service_rejections() ||
        !test_profile_mode() ||
        !test_profile_rejections() || !test_conninfo_policy() ||
        !test_reference_without_provider()) {
        return 1;
    }
    (void)printf("connection_string_tests=passed\n");
    return 0;
}
