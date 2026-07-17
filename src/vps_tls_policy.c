#include "vps_tls_policy.h"

#include <string.h>

static int vps_tls_text_equal(const char *value, const char *expected)
{
    size_t index;
    size_t expected_length;
    if (value == NULL || expected == NULL) return 0;
    expected_length = strlen(expected);
    for (index = 0U; index < expected_length; ++index) {
        char current = value[index];
        if (current >= 'A' && current <= 'Z') current = (char)(current - 'A' + 'a');
        if (current != expected[index]) return 0;
    }
    return value[expected_length] == '\0';
}

static VpsTlsResult vps_tls_parse_mode(const char *value, VpsTlsMode *mode)
{
    if (value == NULL || vps_tls_text_equal(value, "verify-full")) {
        *mode = VPS_TLS_MODE_VERIFY_FULL;
    } else if (vps_tls_text_equal(value, "verify-ca")) {
        *mode = VPS_TLS_MODE_VERIFY_CA;
    } else if (vps_tls_text_equal(value, "require")) {
        *mode = VPS_TLS_MODE_REQUIRE;
    } else if (vps_tls_text_equal(value, "prefer")) {
        *mode = VPS_TLS_MODE_PREFER;
    } else if (vps_tls_text_equal(value, "disable")) {
        *mode = VPS_TLS_MODE_DISABLE;
    } else {
        return VPS_TLS_INVALID_SSLMODE;
    }
    return VPS_TLS_OK;
}

static VpsTlsResult vps_tls_parse_channel_binding(
    const char *value,
    VpsChannelBindingMode *mode)
{
    if (value == NULL || vps_tls_text_equal(value, "prefer")) {
        *mode = VPS_CHANNEL_BINDING_PREFER;
    } else if (vps_tls_text_equal(value, "require")) {
        *mode = VPS_CHANNEL_BINDING_REQUIRE;
    } else if (vps_tls_text_equal(value, "disable")) {
        *mode = VPS_CHANNEL_BINDING_DISABLE;
    } else {
        return VPS_TLS_INVALID_CHANNEL_BINDING;
    }
    return VPS_TLS_OK;
}

static void vps_tls_log(VpsLogger *logger,
                        const VpsTlsPolicy *policy,
                        const VpsTlsOutcome *outcome,
                        VpsTlsResult result)
{
    static const char operation[] = "tls_policy";
    static const char phase[] = "post_connect";
    VpsLogEvent event;
    const char *mode;
    const char *certificate;
    const char *channel_binding;
    const char *status;
    VpsLogLevel level = result == VPS_TLS_OK ? VPS_LOG_LEVEL_INFO
                                             : VPS_LOG_LEVEL_WARN;
    if (logger == NULL || policy == NULL || outcome == NULL) return;
    mode = vps_tls_mode_name(policy->mode);
    certificate = vps_tls_certificate_status_name(outcome->certificate_status);
    channel_binding =
        vps_channel_binding_status_name(outcome->channel_binding_status);
    status = vps_tls_result_name(result);
    if (vps_log_event_init(&event, level) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 sizeof(phase) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_TLS_MODE, mode,
                                 strlen(mode)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_SSL_IN_USE,
                                 (uint64_t)(outcome->ssl_in_use != 0)) !=
            VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_CERTIFICATE_STATUS,
                                 certificate, strlen(certificate)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_CHANNEL_BINDING_STATUS,
                                 channel_binding, strlen(channel_binding)) !=
            VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK) return;
    vps_logger_emit(logger, &event);
}

VpsTlsResult vps_tls_policy_from_config(
    const VpsCredentialConfig *config,
    const VpsTlsPolicyOptions *options,
    VpsTlsPolicy *policy)
{
    VpsTlsPolicy replacement;
    VpsTlsResult result;
    if (config == NULL || options == NULL || policy == NULL) {
        return VPS_TLS_INVALID_ARGUMENT;
    }
    (void)memset(&replacement, 0, sizeof(replacement));
    result = vps_tls_parse_mode(config->sslmode, &replacement.mode);
    if (result != VPS_TLS_OK) return result;
    result = vps_tls_parse_channel_binding(config->channel_binding,
                                           &replacement.channel_binding);
    if (result != VPS_TLS_OK) return result;
    if (replacement.mode == VPS_TLS_MODE_DISABLE &&
        !options->allow_explicit_disable) {
        return VPS_TLS_EXPLICIT_DISABLE_REQUIRED;
    }
    if (replacement.channel_binding == VPS_CHANNEL_BINDING_REQUIRE &&
        replacement.mode == VPS_TLS_MODE_DISABLE) {
        return VPS_TLS_CHANNEL_BINDING_REQUIRED;
    }
    replacement.initialized = 1;
    *policy = replacement;
    return VPS_TLS_OK;
}

VpsTlsResult vps_tls_policy_verify(const VpsTlsPolicy *policy,
                                   const VpsTlsObservation *observation,
                                   VpsTlsOutcome *outcome,
                                   VpsLogger *logger)
{
    VpsTlsOutcome replacement;
    VpsTlsResult result = VPS_TLS_OK;
    if (policy == NULL || !policy->initialized || observation == NULL ||
        outcome == NULL) return VPS_TLS_INVALID_ARGUMENT;
    (void)memset(&replacement, 0, sizeof(replacement));
    replacement.mode = policy->mode;
    replacement.ssl_in_use = observation->ssl_in_use != 0;
    replacement.diagnostic_attributes_complete =
        observation->ssl_library_reported && observation->protocol_reported &&
        observation->cipher_reported;
    replacement.channel_binding_status = policy->channel_binding ==
                                                  VPS_CHANNEL_BINDING_DISABLE
                                              ? VPS_CHANNEL_BINDING_STATUS_DISABLED
                                              : VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED;
    if (!observation->connection_ready) {
        result = VPS_TLS_CONNECTION_NOT_READY;
    } else if (!observation->policy_applied) {
        result = VPS_TLS_POLICY_MISMATCH;
    } else if (policy->mode == VPS_TLS_MODE_DISABLE &&
               observation->ssl_in_use) {
        result = VPS_TLS_FORBIDDEN;
    } else if (policy->mode != VPS_TLS_MODE_DISABLE &&
               policy->mode != VPS_TLS_MODE_PREFER &&
               !observation->ssl_in_use) {
        result = VPS_TLS_REQUIRED;
    } else if (policy->channel_binding == VPS_CHANNEL_BINDING_REQUIRE &&
               !observation->ssl_in_use) {
        replacement.channel_binding_status =
            VPS_CHANNEL_BINDING_STATUS_UNSATISFIED;
        result = VPS_TLS_CHANNEL_BINDING_REQUIRED;
    }
    if (observation->ssl_in_use) {
        if (policy->mode == VPS_TLS_MODE_VERIFY_FULL) {
            replacement.certificate_status =
                VPS_TLS_CERTIFICATE_HOSTNAME_VERIFIED;
        } else if (policy->mode == VPS_TLS_MODE_VERIFY_CA) {
            replacement.certificate_status = VPS_TLS_CERTIFICATE_CA_VERIFIED;
        } else {
            replacement.certificate_status =
                VPS_TLS_CERTIFICATE_ENCRYPTED_UNVERIFIED;
        }
        if (policy->channel_binding == VPS_CHANNEL_BINDING_REQUIRE) {
            replacement.channel_binding_status =
                VPS_CHANNEL_BINDING_STATUS_SATISFIED;
        }
    }
    *outcome = replacement;
    vps_tls_log(logger, policy, &replacement, result);
    return result;
}

const char *vps_tls_mode_name(VpsTlsMode mode)
{
    switch (mode) {
    case VPS_TLS_MODE_VERIFY_FULL: return "verify-full";
    case VPS_TLS_MODE_VERIFY_CA: return "verify-ca";
    case VPS_TLS_MODE_REQUIRE: return "require";
    case VPS_TLS_MODE_PREFER: return "prefer";
    case VPS_TLS_MODE_DISABLE: return "disable";
    default: return "unknown";
    }
}

const char *vps_channel_binding_mode_name(VpsChannelBindingMode mode)
{
    switch (mode) {
    case VPS_CHANNEL_BINDING_PREFER: return "prefer";
    case VPS_CHANNEL_BINDING_REQUIRE: return "require";
    case VPS_CHANNEL_BINDING_DISABLE: return "disable";
    default: return "unknown";
    }
}

const char *vps_tls_certificate_status_name(VpsTlsCertificateStatus status)
{
    switch (status) {
    case VPS_TLS_CERTIFICATE_NONE: return "none";
    case VPS_TLS_CERTIFICATE_ENCRYPTED_UNVERIFIED: return "encrypted_unverified";
    case VPS_TLS_CERTIFICATE_CA_VERIFIED: return "ca_verified";
    case VPS_TLS_CERTIFICATE_HOSTNAME_VERIFIED: return "hostname_verified";
    default: return "unknown";
    }
}

const char *vps_channel_binding_status_name(VpsChannelBindingStatus status)
{
    switch (status) {
    case VPS_CHANNEL_BINDING_STATUS_DISABLED: return "disabled";
    case VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED: return "not_required";
    case VPS_CHANNEL_BINDING_STATUS_SATISFIED: return "satisfied";
    case VPS_CHANNEL_BINDING_STATUS_UNSATISFIED: return "unsatisfied";
    default: return "unknown";
    }
}

const char *vps_tls_result_name(VpsTlsResult result)
{
    switch (result) {
    case VPS_TLS_OK: return "ok";
    case VPS_TLS_INVALID_ARGUMENT: return "invalid_argument";
    case VPS_TLS_INVALID_SSLMODE: return "invalid_sslmode";
    case VPS_TLS_INVALID_CHANNEL_BINDING: return "invalid_channel_binding";
    case VPS_TLS_EXPLICIT_DISABLE_REQUIRED: return "explicit_disable_required";
    case VPS_TLS_CONNECTION_NOT_READY: return "connection_not_ready";
    case VPS_TLS_REQUIRED: return "tls_required";
    case VPS_TLS_FORBIDDEN: return "tls_forbidden";
    case VPS_TLS_CHANNEL_BINDING_REQUIRED: return "channel_binding_required";
    case VPS_TLS_POLICY_MISMATCH: return "policy_mismatch";
    default: return "unknown";
    }
}
