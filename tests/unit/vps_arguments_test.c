#include "vps_arguments.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CHECK(condition, name)                                      \
    do {                                                                 \
        if (!(condition)) {                                              \
            (void)fprintf(stderr, "arguments_case=%s status=failed\n",  \
                          (name));                                       \
            return 0;                                                    \
        }                                                                \
    } while (0)

#define INPUT_LITERAL(value) {value, sizeof(value) - 1U}

typedef struct TestPlatformState {
    size_t zero_calls;
} TestPlatformState;

typedef struct LogCapture {
    size_t events;
    size_t argument_fields;
    size_t presence_fields;
    int sensitive_value_seen;
} LogCapture;

static TestPlatformState *g_platform_state;

static VpsPlatformStatus test_secure_zero(void *buffer, size_t buffer_size)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;
    size_t index;

    if (g_platform_state == NULL ||
        (buffer == NULL && buffer_size != 0U)) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    g_platform_state->zero_calls += 1U;
    for (index = 0U; index < buffer_size; ++index) {
        bytes[index] = 0U;
    }
    return VPS_PLATFORM_OK;
}

static VpsPlatformOperations test_operations(void)
{
    VpsPlatformOperations operations;

    (void)memset(&operations, 0, sizeof(operations));
    operations.structure_size = (uint32_t)sizeof(operations);
    operations.contract_version = VPS_PLATFORM_CONTRACT_VERSION;
    operations.capabilities = VPS_PLATFORM_CAP_SECURE_ZERO;
    operations.secure_zero = test_secure_zero;
    return operations;
}

static int capture_log(void *context, const VpsLogEvent *event)
{
    static const char forbidden_marker[] = "synthetic_conn_marker";
    LogCapture *capture = (LogCapture *)context;
    size_t index;

    capture->events += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->key == VPS_LOG_FIELD_ARGUMENT) {
            capture->argument_fields += 1U;
        } else if (field->key == VPS_LOG_FIELD_PRESENCE_MASK) {
            capture->presence_fields += 1U;
        }
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            field->value.string_value.length ==
                sizeof(forbidden_marker) - 1U &&
            memcmp(field->value.string_value.data, forbidden_marker,
                   sizeof(forbidden_marker) - 1U) == 0) {
            capture->sensitive_value_seen = 1;
        }
    }
    return 0;
}

static int initialize_arguments(VpsParsedArguments *arguments,
                                VpsAllocator *allocator,
                                const VpsPlatformOperations *operations,
                                VpsLogger *logger)
{
    return vps_allocator_system(allocator) == VPS_MEMORY_OK &&
           vps_arguments_init(arguments, allocator, operations, logger) ==
               VPS_ARGUMENTS_OK;
}

static int test_valid_table_and_types(void)
{
    static const VpsArgumentInput inputs[] = {
        INPUT_LITERAL(" CREDENTIAL_REF = 'app/ref' "),
        INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema='catalog'"),
        INPUT_LITERAL("table='product''items'"),
        INPUT_LITERAL("mode=rw"),
        INPUT_LITERAL("allow_view=true"),
        INPUT_LITERAL("allow_materialized_view=false"),
        INPUT_LITERAL("allow_foreign_table=true"),
        INPUT_LITERAL("key_columns='id,tenant_id'"),
        INPUT_LITERAL("optimistic_lock=xmin"),
        INPUT_LITERAL("version_column=row_version"),
        INPUT_LITERAL("geometry=ewkb"),
        INPUT_LITERAL("srid=4294967295"),
        INPUT_LITERAL("connect_timeout=30"),
        INPUT_LITERAL("statement_timeout=30000"),
        INPUT_LITERAL("lock_timeout=5000"),
        INPUT_LITERAL("pool_min=1"),
        INPUT_LITERAL("pool_max=8"),
        INPUT_LITERAL("pool_idle_timeout=300"),
        INPUT_LITERAL("pool_wait_timeout=30"),
        INPUT_LITERAL("pool_validation_interval=30"),
        INPUT_LITERAL("pool_reset=strict_reset"),
        INPUT_LITERAL("pool_readonly_separate=false"),
        INPUT_LITERAL("metadata_mode=cached"),
        INPUT_LITERAL("schema_policy=refresh"),
        INPUT_LITERAL("isolation=serializable"),
        INPUT_LITERAL("transaction_read_only=true")};
    TestPlatformState platform_state;
    VpsPlatformOperations operations = test_operations();
    VpsAllocator allocator;
    VpsParsedArguments arguments;
    VpsArgumentsDiagnostic diagnostic;
    const VpsArgumentValue *value;
    const char *text;
    size_t length;

    (void)memset(&platform_state, 0, sizeof(platform_state));
    g_platform_state = &platform_state;
    TEST_CHECK(initialize_arguments(&arguments, &allocator, &operations, NULL),
               "valid_init");
    TEST_CHECK(vps_arguments_parse(&arguments, inputs,
                                   sizeof(inputs) / sizeof(inputs[0]),
                                   &diagnostic) == VPS_ARGUMENTS_OK &&
                   diagnostic.result == VPS_ARGUMENTS_OK,
               "valid_parse");
    text = vps_argument_text(&arguments, VPS_ARGUMENT_ID_TABLE, &length);
    TEST_CHECK(text != NULL && length == 13U &&
                   memcmp(text, "product'items", length) == 0,
               "quoted_value");
    value = vps_arguments_get(&arguments, VPS_ARGUMENT_ID_SRID);
    TEST_CHECK(value != NULL && value->uint32_value == UINT32_MAX,
               "uint32_boundary");
    value = vps_arguments_get(&arguments,
                              VPS_ARGUMENT_ID_POOL_READONLY_SEPARATE);
    TEST_CHECK(value != NULL && !value->boolean_value,
               "boolean_value");
    value = vps_arguments_get(&arguments, VPS_ARGUMENT_ID_GEOMETRY);
    TEST_CHECK(value != NULL &&
                   value->enum_value == VPS_ARGUMENT_ENUM_GEOMETRY_EWKB,
               "enum_value");
    value = vps_arguments_get(&arguments, VPS_ARGUMENT_ID_ISOLATION);
    TEST_CHECK(value != NULL &&
                   value->enum_value ==
                       VPS_ARGUMENT_ENUM_ISOLATION_SERIALIZABLE,
               "transaction_isolation");
    value = vps_arguments_get(&arguments, VPS_ARGUMENT_ID_SCHEMA_POLICY);
    TEST_CHECK(value != NULL &&
                   value->enum_value == VPS_ARGUMENT_ENUM_SCHEMA_REFRESH,
               "schema_policy");
    value = vps_arguments_get(&arguments,
                              VPS_ARGUMENT_ID_TRANSACTION_READ_ONLY);
    TEST_CHECK(value != NULL && value->boolean_value,
               "transaction_read_only");
    TEST_CHECK(vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "valid_reset");
    TEST_CHECK(vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK &&
                   platform_state.zero_calls == 1U,
               "valid_repeated_reset");
    return 1;
}

static int test_connection_modes(void)
{
    static const VpsArgumentInput service_inputs[] = {
        INPUT_LITERAL("service=app_ro"),
        INPUT_LITERAL("service_file='C:/config/service.conf'"),
        INPUT_LITERAL("source=table"), INPUT_LITERAL("schema=s"),
        INPUT_LITERAL("table=t")};
    static const VpsArgumentInput profile_inputs[] = {
        INPUT_LITERAL("profile=local"), INPUT_LITERAL("source=query"),
        INPUT_LITERAL("query_profile=approved_report"),
        INPUT_LITERAL("query_indexes=by_id=id"),
        INPUT_LITERAL("query_materialization=memory"),
        INPUT_LITERAL("mode=ro")};
    static const VpsArgumentInput connstr_inputs[] = {
        INPUT_LITERAL("connstr=synthetic_conn_marker"),
        INPUT_LITERAL("source=query"), INPUT_LITERAL("query='SELECT 1'"),
        INPUT_LITERAL("mode=ro")};
    TestPlatformState platform_state;
    LogCapture capture;
    VpsPlatformOperations operations = test_operations();
    VpsAllocator allocator;
    VpsParsedArguments arguments;
    VpsLogger logger;

    (void)memset(&platform_state, 0, sizeof(platform_state));
    (void)memset(&capture, 0, sizeof(capture));
    g_platform_state = &platform_state;
    TEST_CHECK(vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG, capture_log,
                               &capture) == VPS_LOG_OK &&
                   initialize_arguments(&arguments, &allocator, &operations,
                                        &logger),
               "modes_init");
    TEST_CHECK(vps_arguments_parse(&arguments, service_inputs,
                                   sizeof(service_inputs) /
                                       sizeof(service_inputs[0]),
                                   NULL) == VPS_ARGUMENTS_OK,
               "service_mode");
    TEST_CHECK(vps_arguments_parse(&arguments, profile_inputs,
                                   sizeof(profile_inputs) /
                                       sizeof(profile_inputs[0]),
                                   NULL) == VPS_ARGUMENTS_OK,
               "profile_mode");
    TEST_CHECK(vps_arguments_get(
                   &arguments, VPS_ARGUMENT_ID_QUERY_MATERIALIZATION)
                       ->enum_value == VPS_ARGUMENT_ENUM_MATERIALIZATION_MEMORY,
               "materialization_mode");
    TEST_CHECK(vps_arguments_parse(&arguments, connstr_inputs,
                                   sizeof(connstr_inputs) /
                                       sizeof(connstr_inputs[0]),
                                   NULL) == VPS_ARGUMENTS_OK,
               "connstr_mode");
    TEST_CHECK(capture.events >= 3U && capture.argument_fields >= 3U &&
                   capture.presence_fields >= 3U &&
                   !capture.sensitive_value_seen,
               "redacted_logging");
    TEST_CHECK(vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "modes_reset");
    return 1;
}

static int test_rejections_preserve_previous(void)
{
    static const VpsArgumentInput valid[] = {
        INPUT_LITERAL("credential_ref=ref"), INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema=stable"), INPUT_LITERAL("table=t")};
    static const VpsArgumentInput duplicate[] = {
        INPUT_LITERAL("credential_ref=ref"),
        INPUT_LITERAL("credential_ref=other"),
        INPUT_LITERAL("source=table"), INPUT_LITERAL("schema=s"),
        INPUT_LITERAL("table=t")};
    static const VpsArgumentInput unknown[] = {
        INPUT_LITERAL("credential_ref=ref"), INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema=s"), INPUT_LITERAL("table=t"),
        INPUT_LITERAL("mystery=value")};
    static const VpsArgumentInput multiple_modes[] = {
        INPUT_LITERAL("credential_ref=ref"), INPUT_LITERAL("service=svc"),
        INPUT_LITERAL("source=table"), INPUT_LITERAL("schema=s"),
        INPUT_LITERAL("table=t")};
    static const VpsArgumentInput query_rw[] = {
        INPUT_LITERAL("profile=p"), INPUT_LITERAL("source=query"),
        INPUT_LITERAL("query='SELECT 1'"), INPUT_LITERAL("mode=rw")};
    TestPlatformState platform_state;
    VpsPlatformOperations operations = test_operations();
    VpsAllocator allocator;
    VpsParsedArguments arguments;
    VpsArgumentsDiagnostic diagnostic;
    const char *text;
    size_t length;

    (void)memset(&platform_state, 0, sizeof(platform_state));
    g_platform_state = &platform_state;
    TEST_CHECK(initialize_arguments(&arguments, &allocator, &operations, NULL) &&
                   vps_arguments_parse(&arguments, valid,
                                       sizeof(valid) / sizeof(valid[0]),
                                       NULL) == VPS_ARGUMENTS_OK,
               "preserve_setup");
    TEST_CHECK(vps_arguments_parse(&arguments, duplicate,
                                   sizeof(duplicate) / sizeof(duplicate[0]),
                                   &diagnostic) ==
                   VPS_ARGUMENTS_DUPLICATE_ARGUMENT &&
                   diagnostic.argument_id ==
                       VPS_ARGUMENT_ID_CREDENTIAL_REF &&
                   diagnostic.sensitive == 0,
               "duplicate_rejected");
    TEST_CHECK(vps_arguments_parse(&arguments, unknown,
                                   sizeof(unknown) / sizeof(unknown[0]),
                                   NULL) == VPS_ARGUMENTS_UNKNOWN_ARGUMENT,
               "unknown_rejected");
    TEST_CHECK(vps_arguments_parse(&arguments, multiple_modes,
                                   sizeof(multiple_modes) /
                                       sizeof(multiple_modes[0]),
                                   NULL) == VPS_ARGUMENTS_INCOMPATIBLE,
               "multiple_modes_rejected");
    TEST_CHECK(vps_arguments_parse(&arguments, query_rw,
                                   sizeof(query_rw) / sizeof(query_rw[0]),
                                   NULL) == VPS_ARGUMENTS_INCOMPATIBLE,
               "query_rw_rejected");
    text = vps_argument_text(&arguments, VPS_ARGUMENT_ID_SCHEMA, &length);
    TEST_CHECK(text != NULL && length == 6U &&
                   memcmp(text, "stable", length) == 0,
               "previous_preserved");
    TEST_CHECK(vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "preserve_reset");
    return 1;
}

static int test_malformed_range_and_utf8(void)
{
    static const char embedded_nul[] = "table=a\0b";
    static const unsigned char invalid_utf8[] = {
        't', 'a', 'b', 'l', 'e', '=', 0xc0U, 0xafU};
    static const VpsArgumentInput overflow[] = {
        INPUT_LITERAL("credential_ref=ref"), INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema=s"), INPUT_LITERAL("table=t"),
        INPUT_LITERAL("pool_max=4294967296")};
    static const VpsArgumentInput malformed_quote[] = {
        INPUT_LITERAL("credential_ref=ref"), INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema='unterminated"), INPUT_LITERAL("table=t")};
    VpsArgumentInput bad_nul[] = {
        INPUT_LITERAL("credential_ref=ref"), INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema=s"),
        {embedded_nul, sizeof(embedded_nul) - 1U}};
    VpsArgumentInput bad_utf8[] = {
        INPUT_LITERAL("credential_ref=ref"), INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema=s"),
        {(const char *)invalid_utf8, sizeof(invalid_utf8)}};
    TestPlatformState platform_state;
    VpsPlatformOperations operations = test_operations();
    VpsAllocator allocator;
    VpsParsedArguments arguments;

    (void)memset(&platform_state, 0, sizeof(platform_state));
    g_platform_state = &platform_state;
    TEST_CHECK(initialize_arguments(&arguments, &allocator, &operations, NULL),
               "malformed_init");
    TEST_CHECK(vps_arguments_parse(&arguments, overflow,
                                   sizeof(overflow) / sizeof(overflow[0]),
                                   NULL) == VPS_ARGUMENTS_RANGE_ERROR,
               "overflow_rejected");
    TEST_CHECK(vps_arguments_parse(&arguments, malformed_quote,
                                   sizeof(malformed_quote) /
                                       sizeof(malformed_quote[0]),
                                   NULL) == VPS_ARGUMENTS_MALFORMED,
               "quote_rejected");
    TEST_CHECK(vps_arguments_parse(&arguments, bad_nul,
                                   sizeof(bad_nul) / sizeof(bad_nul[0]),
                                   NULL) == VPS_ARGUMENTS_MALFORMED,
               "nul_rejected");
    TEST_CHECK(vps_arguments_parse(&arguments, bad_utf8,
                                   sizeof(bad_utf8) / sizeof(bad_utf8[0]),
                                   NULL) == VPS_ARGUMENTS_MALFORMED,
               "utf8_rejected");
    TEST_CHECK(vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "malformed_reset");
    return 1;
}

static int test_fault_allocation_and_definition_table(void)
{
    static const VpsArgumentInput inputs[] = {
        INPUT_LITERAL("credential_ref=ref"), INPUT_LITERAL("source=table"),
        INPUT_LITERAL("schema=s"), INPUT_LITERAL("table=t")};
    TestPlatformState platform_state;
    VpsPlatformOperations operations = test_operations();
    VpsAllocator system_allocator;
    VpsAllocator fault_allocator;
    VpsFaultAllocator fault;
    VpsParsedArguments arguments;
    size_t index;

    (void)memset(&platform_state, 0, sizeof(platform_state));
    g_platform_state = &platform_state;
    TEST_CHECK(vps_allocator_system(&system_allocator) == VPS_MEMORY_OK &&
                   vps_fault_allocator_init(&fault, &system_allocator, 1U) ==
                       VPS_MEMORY_OK &&
                   vps_fault_allocator_make(&fault, &fault_allocator) ==
                       VPS_MEMORY_OK &&
                   vps_arguments_init(&arguments, &fault_allocator,
                                      &operations, NULL) == VPS_ARGUMENTS_OK,
               "fault_init");
    TEST_CHECK(vps_arguments_parse(&arguments, inputs,
                                   sizeof(inputs) / sizeof(inputs[0]),
                                   NULL) == VPS_ARGUMENTS_OUT_OF_MEMORY &&
                   fault.attempt_count == 1U &&
                   fault.active_allocations == 0U,
               "fault_oom");
    for (index = 0U; index < VPS_ARGUMENT_ID_COUNT; ++index) {
        TEST_CHECK(strcmp(vps_argument_name((VpsArgumentId)index),
                          "unknown") != 0,
                   "definition_name");
    }
    TEST_CHECK(vps_argument_is_sensitive(VPS_ARGUMENT_ID_CONNSTR) &&
                   vps_argument_is_sensitive(VPS_ARGUMENT_ID_QUERY) &&
                   !vps_argument_is_sensitive(
                       VPS_ARGUMENT_ID_CREDENTIAL_REF),
               "sensitive_classification");
    TEST_CHECK(vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "fault_reset");
    return 1;
}

static int test_string_boundaries(void)
{
    static const char prefix[] = "credential_ref=";
    static const VpsArgumentInput suffix[] = {
        INPUT_LITERAL("source=table"), INPUT_LITERAL("schema=s"),
        INPUT_LITERAL("table=t")};
    char exact_text[sizeof(prefix) - 1U + 255U + 1U];
    char over_text[sizeof(prefix) - 1U + 256U + 1U];
    VpsArgumentInput exact_inputs[4];
    VpsArgumentInput over_inputs[4];
    TestPlatformState platform_state;
    VpsPlatformOperations operations = test_operations();
    VpsAllocator allocator;
    VpsParsedArguments arguments;
    size_t index;

    (void)memcpy(exact_text, prefix, sizeof(prefix) - 1U);
    (void)memset(exact_text + sizeof(prefix) - 1U, 'a', 255U);
    exact_text[sizeof(exact_text) - 1U] = '\0';
    (void)memcpy(over_text, prefix, sizeof(prefix) - 1U);
    (void)memset(over_text + sizeof(prefix) - 1U, 'b', 256U);
    over_text[sizeof(over_text) - 1U] = '\0';
    exact_inputs[0].text = exact_text;
    exact_inputs[0].length = sizeof(exact_text) - 1U;
    over_inputs[0].text = over_text;
    over_inputs[0].length = sizeof(over_text) - 1U;
    for (index = 0U; index < sizeof(suffix) / sizeof(suffix[0]); ++index) {
        exact_inputs[index + 1U] = suffix[index];
        over_inputs[index + 1U] = suffix[index];
    }

    (void)memset(&platform_state, 0, sizeof(platform_state));
    g_platform_state = &platform_state;
    TEST_CHECK(initialize_arguments(&arguments, &allocator, &operations, NULL),
               "boundary_init");
    TEST_CHECK(vps_arguments_parse(&arguments, exact_inputs,
                                   sizeof(exact_inputs) /
                                       sizeof(exact_inputs[0]),
                                   NULL) == VPS_ARGUMENTS_OK,
               "boundary_exact");
    TEST_CHECK(vps_arguments_parse(&arguments, over_inputs,
                                   sizeof(over_inputs) /
                                       sizeof(over_inputs[0]),
                                   NULL) == VPS_ARGUMENTS_LIMIT_EXCEEDED,
               "boundary_over");
    TEST_CHECK(vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "boundary_reset");
    return 1;
}

int main(void)
{
    if (!test_valid_table_and_types() || !test_connection_modes() ||
        !test_rejections_preserve_previous() ||
        !test_malformed_range_and_utf8() ||
        !test_fault_allocation_and_definition_table() ||
        !test_string_boundaries()) {
        return 1;
    }
    (void)fprintf(stdout, "arguments_suite status=passed\n");
    return 0;
}
