#include "vps_tls_policy.h"

#include <stdio.h>
#include <string.h>

#define TEST_CHECK(condition, name)                               \
    do {                                                          \
        if (!(condition)) {                                       \
            (void)fprintf(stderr, "tls_case=%s status=failed\n",  \
                          (name));                                \
            return 0;                                             \
        }                                                         \
    } while (0)

typedef struct TestLogCapture {
    size_t events;
    size_t policy_fields;
    int forbidden_seen;
} TestLogCapture;

typedef struct TestVerifyCase {
    const char *name;
    const char *sslmode;
    const char *channel_binding;
    int allow_disable;
    VpsTlsObservation observation;
    VpsTlsResult expected_result;
    VpsTlsCertificateStatus expected_certificate;
    VpsChannelBindingStatus expected_channel_binding;
} TestVerifyCase;

static const TestVerifyCase test_verify_cases[] = {
    {"verify_full", "verify-full", "prefer", 0,
     {1, 1, 1, 1, 1, 1}, VPS_TLS_OK,
     VPS_TLS_CERTIFICATE_HOSTNAME_VERIFIED,
     VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED},
    {"verify_ca", "verify-ca", "prefer", 0,
     {1, 1, 1, 1, 1, 1}, VPS_TLS_OK, VPS_TLS_CERTIFICATE_CA_VERIFIED,
     VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED},
    {"require", "require", "prefer", 0,
     {1, 1, 1, 1, 1, 1}, VPS_TLS_OK,
     VPS_TLS_CERTIFICATE_ENCRYPTED_UNVERIFIED,
     VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED},
    {"require_downgrade", "require", "prefer", 0,
     {1, 0, 0, 0, 0, 1}, VPS_TLS_REQUIRED, VPS_TLS_CERTIFICATE_NONE,
     VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED},
    {"prefer_tls", "prefer", "prefer", 0,
     {1, 1, 1, 1, 1, 1}, VPS_TLS_OK,
     VPS_TLS_CERTIFICATE_ENCRYPTED_UNVERIFIED,
     VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED},
    {"prefer_plain", "prefer", "prefer", 0,
     {1, 0, 0, 0, 0, 1}, VPS_TLS_OK, VPS_TLS_CERTIFICATE_NONE,
     VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED},
    {"disable", "disable", "disable", 1,
     {1, 0, 0, 0, 0, 1}, VPS_TLS_OK, VPS_TLS_CERTIFICATE_NONE,
     VPS_CHANNEL_BINDING_STATUS_DISABLED},
    {"disable_tls_forbidden", "disable", "disable", 1,
     {1, 1, 1, 1, 1, 1}, VPS_TLS_FORBIDDEN,
     VPS_TLS_CERTIFICATE_ENCRYPTED_UNVERIFIED,
     VPS_CHANNEL_BINDING_STATUS_DISABLED},
    {"binding_satisfied", "verify-full", "require", 0,
     {1, 1, 1, 1, 1, 1}, VPS_TLS_OK,
     VPS_TLS_CERTIFICATE_HOSTNAME_VERIFIED,
     VPS_CHANNEL_BINDING_STATUS_SATISFIED},
    {"binding_unsatisfied", "prefer", "require", 0,
     {1, 0, 0, 0, 0, 1}, VPS_TLS_CHANNEL_BINDING_REQUIRED,
     VPS_TLS_CERTIFICATE_NONE, VPS_CHANNEL_BINDING_STATUS_UNSATISFIED},
    {"connection_failed", "verify-full", "prefer", 0,
     {0, 0, 0, 0, 0, 1}, VPS_TLS_CONNECTION_NOT_READY,
     VPS_TLS_CERTIFICATE_NONE, VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED},
    {"policy_mismatch", "verify-full", "prefer", 0,
     {1, 1, 1, 1, 1, 0}, VPS_TLS_POLICY_MISMATCH,
     VPS_TLS_CERTIFICATE_HOSTNAME_VERIFIED,
     VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED},
    {"attributes_optional", "verify-full", "prefer", 0,
     {1, 1, 0, 0, 0, 1}, VPS_TLS_OK,
     VPS_TLS_CERTIFICATE_HOSTNAME_VERIFIED,
     VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED}};

static int test_log_sink(void *context, const VpsLogEvent *event)
{
    static const char forbidden[] = "C:/private/client.key";
    TestLogCapture *capture = (TestLogCapture *)context;
    size_t index;
    capture->events += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->key == VPS_LOG_FIELD_TLS_MODE ||
            field->key == VPS_LOG_FIELD_SSL_IN_USE ||
            field->key == VPS_LOG_FIELD_CERTIFICATE_STATUS ||
            field->key == VPS_LOG_FIELD_CHANNEL_BINDING_STATUS) {
            capture->policy_fields += 1U;
        }
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            field->value.string_value.length == sizeof(forbidden) - 1U &&
            memcmp(field->value.string_value.data, forbidden,
                   sizeof(forbidden) - 1U) == 0) capture->forbidden_seen = 1;
    }
    return 0;
}

static int test_defaults_and_rejections(void)
{
    VpsCredentialConfig config;
    VpsTlsPolicyOptions options = {0};
    VpsTlsPolicy policy;
    VpsTlsPolicy published;
    (void)memset(&config, 0, sizeof(config));
    (void)memset(&policy, 0x5a, sizeof(policy));
    TEST_CHECK(vps_tls_policy_from_config(&config, &options, &policy) ==
                   VPS_TLS_OK && policy.mode == VPS_TLS_MODE_VERIFY_FULL &&
                   policy.channel_binding == VPS_CHANNEL_BINDING_PREFER,
               "defaults");
    published = policy;
    config.sslmode = "disable";
    TEST_CHECK(vps_tls_policy_from_config(&config, &options, &policy) ==
                       VPS_TLS_EXPLICIT_DISABLE_REQUIRED &&
                   memcmp(&policy, &published, sizeof(policy)) == 0,
               "disable_requires_opt_in");
    options.allow_explicit_disable = 1;
    config.channel_binding = "require";
    TEST_CHECK(vps_tls_policy_from_config(&config, &options, &policy) ==
                       VPS_TLS_CHANNEL_BINDING_REQUIRED &&
                   memcmp(&policy, &published, sizeof(policy)) == 0,
               "disable_binding_conflict");
    config.sslmode = "unknown";
    config.channel_binding = NULL;
    TEST_CHECK(vps_tls_policy_from_config(&config, &options, &policy) ==
                       VPS_TLS_INVALID_SSLMODE &&
                   memcmp(&policy, &published, sizeof(policy)) == 0,
               "unknown_sslmode");
    config.sslmode = "require";
    config.channel_binding = "unknown";
    TEST_CHECK(vps_tls_policy_from_config(&config, &options, &policy) ==
                       VPS_TLS_INVALID_CHANNEL_BINDING &&
                   memcmp(&policy, &published, sizeof(policy)) == 0,
               "unknown_binding");
    TEST_CHECK(vps_tls_policy_from_config(NULL, &options, &policy) ==
                       VPS_TLS_INVALID_ARGUMENT &&
                   vps_tls_policy_from_config(&config, NULL, &policy) ==
                       VPS_TLS_INVALID_ARGUMENT &&
                   vps_tls_policy_from_config(&config, &options, NULL) ==
                       VPS_TLS_INVALID_ARGUMENT,
               "invalid_arguments");
    return 1;
}

static int test_verification_table(void)
{
    VpsLogger logger;
    TestLogCapture capture = {0};
    size_t index;
    TEST_CHECK(vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG, test_log_sink,
                               &capture) == VPS_LOG_OK,
               "logger_init");
    for (index = 0U; index < sizeof(test_verify_cases) /
                                  sizeof(test_verify_cases[0]); ++index) {
        const TestVerifyCase *test = &test_verify_cases[index];
        VpsCredentialConfig config;
        VpsTlsPolicyOptions options;
        VpsTlsPolicy policy;
        VpsTlsOutcome outcome;
        (void)memset(&config, 0, sizeof(config));
        config.sslmode = test->sslmode;
        config.channel_binding = test->channel_binding;
        config.sslkey = "C:/private/client.key";
        options.allow_explicit_disable = test->allow_disable;
        TEST_CHECK(vps_tls_policy_from_config(&config, &options, &policy) ==
                       VPS_TLS_OK,
                   test->name);
        TEST_CHECK(vps_tls_policy_verify(&policy, &test->observation, &outcome,
                                         &logger) == test->expected_result &&
                       outcome.certificate_status == test->expected_certificate &&
                       outcome.channel_binding_status ==
                           test->expected_channel_binding,
                   test->name);
        TEST_CHECK(outcome.diagnostic_attributes_complete ==
                       (test->observation.ssl_library_reported &&
                        test->observation.protocol_reported &&
                        test->observation.cipher_reported),
                   test->name);
    }
    TEST_CHECK(capture.events == sizeof(test_verify_cases) /
                                     sizeof(test_verify_cases[0]) &&
                   capture.policy_fields == capture.events * 4U &&
                   !capture.forbidden_seen,
               "redacted_logging");
    return 1;
}

int main(void)
{
    if (!test_defaults_and_rejections() || !test_verification_table()) return 1;
    (void)printf("tls_policy_tests=passed\n");
    return 0;
}
