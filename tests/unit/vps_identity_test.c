#include "vps_identity.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CHECK(condition, name)                                  \
    do {                                                             \
        if (!(condition)) {                                          \
            (void)fprintf(stderr, "identity_case=%s status=failed\n", \
                          (name));                                   \
            return 0;                                                \
        }                                                            \
    } while (0)

#define INPUT_LITERAL(value) {value, sizeof(value) - 1U}

typedef struct TestLogCapture {
    size_t events;
    size_t fingerprint_fields;
    int secret_seen;
} TestLogCapture;

typedef struct TestFailAllocator {
    int fail;
    size_t active;
} TestFailAllocator;

typedef struct TestFieldCase {
    size_t offset;
    const char *replacement;
    const char *name;
} TestFieldCase;

#define CONFIG_OFFSET(member) offsetof(VpsCredentialConfig, member)

static const TestFieldCase test_field_cases[] = {
    {CONFIG_OFFSET(hosts), "other.example.test", "hosts"},
    {CONFIG_OFFSET(ports), "6432", "ports"},
    {CONFIG_OFFSET(dbname), "otherdb", "dbname"},
    {CONFIG_OFFSET(user), "otheruser", "user"},
    {CONFIG_OFFSET(service), "other_service", "service"},
    {CONFIG_OFFSET(service_file), "C:/services/other.conf", "service_file"},
    {CONFIG_OFFSET(sslmode), "require", "sslmode"},
    {CONFIG_OFFSET(sslrootcert), "C:/certs/other-ca.pem", "sslrootcert"},
    {CONFIG_OFFSET(sslcert), "C:/certs/other-client.pem", "sslcert"},
    {CONFIG_OFFSET(sslkey), "C:/keys/other-client.key", "sslkey"},
    {CONFIG_OFFSET(sslcrl), "C:/certs/other.crl", "sslcrl"},
    {CONFIG_OFFSET(channel_binding), "require", "channel_binding"},
    {CONFIG_OFFSET(target_session_attrs), "read-write", "target_attrs"},
    {CONFIG_OFFSET(search_path), "private,pg_catalog", "search_path"},
    {CONFIG_OFFSET(connect_timeout), "31", "connect_timeout"},
    {CONFIG_OFFSET(statement_timeout), "301", "statement_timeout"},
    {CONFIG_OFFSET(lock_timeout), "41", "lock_timeout"}};

static int test_log_sink(void *context, const VpsLogEvent *event)
{
    static const char secret[] = "opaque-identity-secret";
    TestLogCapture *capture = (TestLogCapture *)context;
    size_t index;
    capture->events += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->key == VPS_LOG_FIELD_CONNECTION_FINGERPRINT) {
            capture->fingerprint_fields += 1U;
        }
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            field->value.string_value.length == sizeof(secret) - 1U &&
            memcmp(field->value.string_value.data, secret,
                   sizeof(secret) - 1U) == 0) {
            capture->secret_seen = 1;
        }
    }
    return 0;
}

static void *test_fail_allocate(void *context, size_t size)
{
    TestFailAllocator *state = (TestFailAllocator *)context;
    void *memory;
    if (state->fail) return NULL;
    memory = malloc(size);
    if (memory != NULL) state->active += 1U;
    return memory;
}

static void *test_fail_reallocate(void *context,
                                  void *memory,
                                  size_t old_size,
                                  size_t new_size)
{
    TestFailAllocator *state = (TestFailAllocator *)context;
    void *replacement;
    (void)old_size;
    if (state->fail) return NULL;
    replacement = realloc(memory, new_size);
    if (replacement != NULL && memory == NULL) state->active += 1U;
    return replacement;
}

static void test_fail_deallocate(void *context, void *memory, size_t size)
{
    TestFailAllocator *state = (TestFailAllocator *)context;
    (void)size;
    if (memory != NULL) {
        state->active -= 1U;
        free(memory);
    }
}

static void test_config_init(VpsConnectionConfig *connection)
{
    (void)memset(connection, 0, sizeof(*connection));
    connection->config.header.structure_size =
        (uint32_t)sizeof(connection->config);
    connection->config.header.api_version = VPS_API_VERSION;
    connection->config.header.present_fields = VPS_CREDENTIAL_FIELDS_CURRENT;
    connection->config.hosts = "DB.Example.Test";
    connection->config.ports = "05432";
    connection->config.user = "reader";
    connection->config.password = "opaque-identity-secret";
    connection->config.dbname = "catalog";
    connection->config.service = "reporting";
    connection->config.service_file = "C:\\services\\pg_service.conf";
    connection->config.sslmode = "VERIFY-FULL";
    connection->config.sslrootcert = "C:\\certs\\.\\ca.pem";
    connection->config.sslcert = "C:\\certs\\client.pem";
    connection->config.sslkey = "C:\\keys\\client.key";
    connection->config.sslcrl = "C:\\certs\\revocations.crl";
    connection->config.channel_binding = "PREFER";
    connection->config.target_session_attrs = "ANY";
    connection->config.connect_timeout = "0030";
    connection->config.statement_timeout = "0300";
    connection->config.lock_timeout = "0040";
    connection->config.application_name = "ignored-application";
    connection->config.search_path = " public , pg_catalog ";
    connection->generation = 7U;
    connection->initialized = 1;
}

static int test_arguments_init(VpsParsedArguments *arguments,
                               const VpsAllocator *allocator,
                               VpsLogger *logger,
                               const char *mode)
{
    VpsArgumentInput inputs[] = {
        INPUT_LITERAL("credential_ref=identity/ref"),
        INPUT_LITERAL("source=table"), INPUT_LITERAL("schema=public"),
        INPUT_LITERAL("table=items"), {NULL, 0U}};
    char mode_input[16];
    VpsArgumentsDiagnostic diagnostic;
    size_t count = 4U;
    if (mode != NULL) {
        int written = snprintf(mode_input, sizeof(mode_input), "mode=%s", mode);
        if (written <= 0 || (size_t)written >= sizeof(mode_input)) return 0;
        inputs[count].text = mode_input;
        inputs[count].length = (size_t)written;
        count += 1U;
    }
    return vps_arguments_init(arguments, allocator,
                              vps_platform_current_operations(), logger) ==
               VPS_ARGUMENTS_OK &&
           vps_arguments_parse(arguments, inputs, count, &diagnostic) ==
               VPS_ARGUMENTS_OK;
}

static int test_contains(const unsigned char *data,
                         size_t data_length,
                         const char *needle)
{
    size_t needle_length = strlen(needle);
    size_t index;
    if (needle_length > data_length) return 0;
    for (index = 0U; index <= data_length - needle_length; ++index) {
        if (memcmp(data + index, needle, needle_length) == 0) return 1;
    }
    return 0;
}

static int test_equivalent_and_generation(void)
{
    VpsAllocator allocator;
    VpsLogger logger;
    TestLogCapture capture = {0};
    VpsParsedArguments arguments;
    VpsConnectionConfig first_config;
    VpsConnectionConfig second_config;
    VpsConnectionIdentity first;
    VpsConnectionIdentity second;
    VpsIdentityBuildOptions first_options = {"PostgreSQL", 10U, 7U, 11U};
    VpsIdentityBuildOptions second_options = {NULL, 0U, 7U, 11U};
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG, test_log_sink,
                                   &capture) == VPS_LOG_OK &&
                   test_arguments_init(&arguments, &allocator, &logger, NULL),
               "equivalent_init");
    test_config_init(&first_config);
    test_config_init(&second_config);
    second_config.config.hosts = "db.example.test";
    second_config.config.ports = "5432";
    second_config.config.sslmode = "verify-full";
    second_config.config.service_file = "c:/services/pg_service.conf";
    second_config.config.sslrootcert = "c:/certs/ca.pem";
    second_config.config.sslcert = "c:/certs/client.pem";
    second_config.config.sslkey = "c:/keys/client.key";
    second_config.config.sslcrl = "c:/certs/revocations.crl";
    second_config.config.channel_binding = "prefer";
    second_config.config.target_session_attrs = "any";
    second_config.config.connect_timeout = "30";
    second_config.config.statement_timeout = "300";
    second_config.config.lock_timeout = "40";
    second_config.config.search_path = "public,pg_catalog";
    TEST_CHECK(vps_identity_init(&first, &allocator, &logger) == VPS_IDENTITY_OK &&
                   vps_identity_init(&second, &allocator, &logger) == VPS_IDENTITY_OK,
               "equivalent_identity_init");
    TEST_CHECK(vps_identity_compare(&first, &second) == VPS_IDENTITY_DIFFERENT &&
                   vps_identity_fingerprint(&first) == NULL,
               "unbuilt_not_equal");
    TEST_CHECK(vps_identity_build(&first, &first_config, &arguments,
                                  &first_options) == VPS_IDENTITY_OK &&
                   vps_identity_build(&second, &second_config, &arguments,
                                      &second_options) == VPS_IDENTITY_OK,
               "equivalent_build");
    TEST_CHECK(vps_identity_compare(&first, &second) == VPS_IDENTITY_SAME &&
                   strcmp(vps_identity_fingerprint(&first),
                          vps_identity_fingerprint(&second)) == 0,
               "equivalent_same");
    TEST_CHECK(strlen(vps_identity_fingerprint(&first)) ==
                   VPS_IDENTITY_FINGERPRINT_LENGTH &&
                   !test_contains(first.canonical.data, first.canonical.size,
                                  "opaque-identity-secret") &&
                   !test_contains(first.canonical.data, first.canonical.size,
                                  "identity/ref") &&
                   !test_contains(first.canonical.data, first.canonical.size,
                                  "ignored-application") &&
                   !capture.secret_seen && capture.fingerprint_fields != 0U,
               "secret_exclusion");
    second_config.config.password = "different-secret";
    second_options.credential_generation = 8U;
    TEST_CHECK(vps_identity_build(&second, &second_config, &arguments,
                                  &second_options) == VPS_IDENTITY_OK &&
                   strcmp(vps_identity_fingerprint(&first),
                          vps_identity_fingerprint(&second)) == 0 &&
                   vps_identity_compare(&first, &second) ==
                       VPS_IDENTITY_GENERATION_CHANGED,
               "password_generation");
    second_options.credential_generation = 7U;
    second_options.configuration_generation = 12U;
    TEST_CHECK(vps_identity_build(&second, &second_config, &arguments,
                                  &second_options) == VPS_IDENTITY_OK &&
                   vps_identity_compare(&first, &second) ==
                       VPS_IDENTITY_GENERATION_CHANGED,
               "service_generation");
    vps_identity_cleanup(&second);
    vps_identity_cleanup(&first);
    TEST_CHECK(vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "equivalent_cleanup");
    return 1;
}

static int test_relevant_field_matrix(void)
{
    VpsAllocator allocator;
    VpsParsedArguments arguments;
    VpsConnectionConfig base_config;
    VpsConnectionConfig changed_config;
    VpsConnectionIdentity base;
    VpsConnectionIdentity changed;
    VpsIdentityBuildOptions options = {NULL, 0U, 9U, 4U};
    size_t index;
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   test_arguments_init(&arguments, &allocator, NULL, NULL) &&
                   vps_identity_init(&base, &allocator, NULL) == VPS_IDENTITY_OK &&
                   vps_identity_init(&changed, &allocator, NULL) == VPS_IDENTITY_OK,
               "field_matrix_init");
    test_config_init(&base_config);
    TEST_CHECK(vps_identity_build(&base, &base_config, &arguments, &options) ==
                   VPS_IDENTITY_OK,
               "field_matrix_base");
    for (index = 0U; index < sizeof(test_field_cases) /
                                  sizeof(test_field_cases[0]); ++index) {
        const char **member;
        changed_config = base_config;
        member = (const char **)((unsigned char *)&changed_config.config +
                                 test_field_cases[index].offset);
        *member = test_field_cases[index].replacement;
        TEST_CHECK(vps_identity_build(&changed, &changed_config, &arguments,
                                      &options) == VPS_IDENTITY_OK,
                   test_field_cases[index].name);
        TEST_CHECK(vps_identity_compare(&base, &changed) ==
                       VPS_IDENTITY_DIFFERENT,
                   test_field_cases[index].name);
    }
    vps_identity_cleanup(&changed);
    vps_identity_cleanup(&base);
    TEST_CHECK(vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK,
               "field_matrix_cleanup");
    return 1;
}

static int test_mode_backend_and_bounds(void)
{
    VpsAllocator allocator;
    VpsParsedArguments ro_arguments;
    VpsParsedArguments rw_arguments;
    VpsConnectionConfig config;
    VpsConnectionIdentity ro;
    VpsConnectionIdentity rw;
    VpsIdentityBuildOptions options = {NULL, 0U, 1U, 1U};
    VpsIdentityBuildOptions other_backend = {"other", 5U, 1U, 1U};
    char overlong[VPS_CREDENTIAL_VALUE_MAX_LENGTH + 2U];
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   test_arguments_init(&ro_arguments, &allocator, NULL, "ro") &&
                   test_arguments_init(&rw_arguments, &allocator, NULL, "rw") &&
                   vps_identity_init(&ro, &allocator, NULL) == VPS_IDENTITY_OK &&
                   vps_identity_init(&rw, &allocator, NULL) == VPS_IDENTITY_OK,
               "mode_init");
    test_config_init(&config);
    TEST_CHECK(vps_identity_build(&ro, &config, &ro_arguments, &options) ==
                   VPS_IDENTITY_OK &&
                   vps_identity_build(&rw, &config, &rw_arguments, &options) ==
                   VPS_IDENTITY_OK &&
                   vps_identity_compare(&ro, &rw) == VPS_IDENTITY_DIFFERENT,
               "read_write_class");
    TEST_CHECK(vps_identity_build(&rw, &config, &ro_arguments, &other_backend) ==
                   VPS_IDENTITY_OK &&
                   vps_identity_compare(&ro, &rw) == VPS_IDENTITY_DIFFERENT,
               "backend_difference");
    (void)memset(overlong, 'a', sizeof(overlong));
    overlong[sizeof(overlong) - 1U] = '\0';
    config.config.hosts = overlong;
    TEST_CHECK(vps_identity_build(&rw, &config, &ro_arguments, &options) ==
                   VPS_IDENTITY_INVALID_VALUE,
               "value_limit");
    config.config.hosts = "host";
    config.config.ports = "65536";
    TEST_CHECK(vps_identity_build(&rw, &config, &ro_arguments, &options) ==
                   VPS_IDENTITY_INVALID_VALUE,
               "port_limit");
    config.config.ports = "5432";
    config.config.connect_timeout = "4294967296";
    TEST_CHECK(vps_identity_build(&rw, &config, &ro_arguments, &options) ==
                   VPS_IDENTITY_INVALID_VALUE,
               "timeout_32bit");
    vps_identity_cleanup(&rw);
    vps_identity_cleanup(&ro);
    TEST_CHECK(vps_arguments_reset(&rw_arguments) == VPS_ARGUMENTS_OK &&
                   vps_arguments_reset(&ro_arguments) == VPS_ARGUMENTS_OK,
               "mode_cleanup");
    return 1;
}

static int test_transaction_policy_identity(void)
{
    static const VpsArgumentInput base_inputs[] = {
        INPUT_LITERAL("credential_ref=identity/ref"),
        INPUT_LITERAL("source=table"), INPUT_LITERAL("schema=public"),
        INPUT_LITERAL("table=items"),
        INPUT_LITERAL("isolation=read_committed"),
        INPUT_LITERAL("transaction_read_only=false")};
    static const VpsArgumentInput changed_inputs[] = {
        INPUT_LITERAL("credential_ref=identity/ref"),
        INPUT_LITERAL("source=table"), INPUT_LITERAL("schema=public"),
        INPUT_LITERAL("table=items"),
        INPUT_LITERAL("isolation=serializable"),
        INPUT_LITERAL("transaction_read_only=true")};
    VpsAllocator allocator;
    VpsParsedArguments base_arguments;
    VpsParsedArguments changed_arguments;
    VpsConnectionConfig config;
    VpsConnectionIdentity base;
    VpsConnectionIdentity changed;
    VpsIdentityBuildOptions options = {NULL, 0U, 1U, 1U};
    TEST_CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                   vps_arguments_init(&base_arguments, &allocator,
                                      vps_platform_current_operations(), NULL) ==
                       VPS_ARGUMENTS_OK &&
                   vps_arguments_init(&changed_arguments, &allocator,
                                      vps_platform_current_operations(), NULL) ==
                       VPS_ARGUMENTS_OK &&
                   vps_arguments_parse(&base_arguments, base_inputs,
                                       sizeof(base_inputs) /
                                           sizeof(base_inputs[0]),
                                       NULL) == VPS_ARGUMENTS_OK &&
                   vps_arguments_parse(&changed_arguments, changed_inputs,
                                       sizeof(changed_inputs) /
                                           sizeof(changed_inputs[0]),
                                       NULL) == VPS_ARGUMENTS_OK &&
                   vps_identity_init(&base, &allocator, NULL) ==
                       VPS_IDENTITY_OK &&
                   vps_identity_init(&changed, &allocator, NULL) ==
                       VPS_IDENTITY_OK,
               "transaction_policy_init");
    test_config_init(&config);
    TEST_CHECK(vps_identity_build(&base, &config, &base_arguments, &options) ==
                       VPS_IDENTITY_OK &&
                   vps_identity_build(&changed, &config, &changed_arguments,
                                      &options) == VPS_IDENTITY_OK &&
                   vps_identity_compare(&base, &changed) ==
                       VPS_IDENTITY_DIFFERENT,
               "transaction_policy_difference");
    vps_identity_cleanup(&changed);
    vps_identity_cleanup(&base);
    TEST_CHECK(vps_arguments_reset(&changed_arguments) == VPS_ARGUMENTS_OK &&
                   vps_arguments_reset(&base_arguments) == VPS_ARGUMENTS_OK,
               "transaction_policy_cleanup");
    return 1;
}

static int test_fault_transaction(void)
{
    TestFailAllocator state = {0};
    VpsAllocator allocator;
    VpsParsedArguments arguments;
    VpsConnectionConfig config;
    VpsConnectionIdentity identity;
    VpsIdentityBuildOptions options = {NULL, 0U, 1U, 1U};
    char preserved[VPS_IDENTITY_FINGERPRINT_BUFFER_SIZE];
    TEST_CHECK(vps_allocator_init(&allocator, VPS_ALLOCATOR_FAMILY_TEST, &state,
                                  test_fail_allocate, test_fail_reallocate,
                                  test_fail_deallocate) == VPS_MEMORY_OK &&
                   test_arguments_init(&arguments, &allocator, NULL, NULL) &&
                   vps_identity_init(&identity, &allocator, NULL) == VPS_IDENTITY_OK,
               "fault_init");
    test_config_init(&config);
    TEST_CHECK(vps_identity_build(&identity, &config, &arguments, &options) ==
                   VPS_IDENTITY_OK,
               "fault_baseline");
    (void)memcpy(preserved, identity.fingerprint, sizeof(preserved));
    state.fail = 1;
    config.config.hosts = "changed.example.test";
    TEST_CHECK(vps_identity_build(&identity, &config, &arguments, &options) ==
                   VPS_IDENTITY_OUT_OF_MEMORY &&
                   memcmp(preserved, identity.fingerprint, sizeof(preserved)) == 0,
               "fault_unchanged");
    state.fail = 0;
    vps_identity_cleanup(&identity);
    TEST_CHECK(vps_arguments_reset(&arguments) == VPS_ARGUMENTS_OK &&
                   state.active == 0U,
               "fault_cleanup");
    return 1;
}

int main(void)
{
    if (!test_equivalent_and_generation() || !test_relevant_field_matrix() ||
        !test_mode_backend_and_bounds() || !test_transaction_policy_identity() ||
        !test_fault_transaction()) {
        return 1;
    }
    (void)printf("identity_tests=passed\n");
    return 0;
}
