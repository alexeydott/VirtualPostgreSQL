#ifndef VPS_TLS_POLICY_H
#define VPS_TLS_POLICY_H

#include "virtualpostgresql/vps_api.h"
#include "vps_logging.h"

typedef enum VpsTlsMode {
    VPS_TLS_MODE_VERIFY_FULL = 0,
    VPS_TLS_MODE_VERIFY_CA = 1,
    VPS_TLS_MODE_REQUIRE = 2,
    VPS_TLS_MODE_PREFER = 3,
    VPS_TLS_MODE_DISABLE = 4
} VpsTlsMode;

typedef enum VpsChannelBindingMode {
    VPS_CHANNEL_BINDING_PREFER = 0,
    VPS_CHANNEL_BINDING_REQUIRE = 1,
    VPS_CHANNEL_BINDING_DISABLE = 2
} VpsChannelBindingMode;

typedef enum VpsTlsCertificateStatus {
    VPS_TLS_CERTIFICATE_NONE = 0,
    VPS_TLS_CERTIFICATE_ENCRYPTED_UNVERIFIED = 1,
    VPS_TLS_CERTIFICATE_CA_VERIFIED = 2,
    VPS_TLS_CERTIFICATE_HOSTNAME_VERIFIED = 3
} VpsTlsCertificateStatus;

typedef enum VpsChannelBindingStatus {
    VPS_CHANNEL_BINDING_STATUS_DISABLED = 0,
    VPS_CHANNEL_BINDING_STATUS_NOT_REQUIRED = 1,
    VPS_CHANNEL_BINDING_STATUS_SATISFIED = 2,
    VPS_CHANNEL_BINDING_STATUS_UNSATISFIED = 3
} VpsChannelBindingStatus;

typedef enum VpsTlsResult {
    VPS_TLS_OK = 0,
    VPS_TLS_INVALID_ARGUMENT = 1,
    VPS_TLS_INVALID_SSLMODE = 2,
    VPS_TLS_INVALID_CHANNEL_BINDING = 3,
    VPS_TLS_EXPLICIT_DISABLE_REQUIRED = 4,
    VPS_TLS_CONNECTION_NOT_READY = 5,
    VPS_TLS_REQUIRED = 6,
    VPS_TLS_FORBIDDEN = 7,
    VPS_TLS_CHANNEL_BINDING_REQUIRED = 8,
    VPS_TLS_POLICY_MISMATCH = 9
} VpsTlsResult;

typedef struct VpsTlsPolicyOptions {
    int allow_explicit_disable;
} VpsTlsPolicyOptions;

typedef struct VpsTlsPolicy {
    VpsTlsMode mode;
    VpsChannelBindingMode channel_binding;
    int initialized;
} VpsTlsPolicy;

/*
 * Observation contains only non-sensitive post-connect facts. A successful
 * libpq connection configured with verify-ca/verify-full is evidence that
 * libpq enforced CA/hostname verification; raw attributes and certificate
 * material never cross this boundary.
 */
typedef struct VpsTlsObservation {
    int connection_ready;
    int ssl_in_use;
    int ssl_library_reported;
    int protocol_reported;
    int cipher_reported;
    int policy_applied;
} VpsTlsObservation;

typedef struct VpsTlsOutcome {
    VpsTlsMode mode;
    VpsTlsCertificateStatus certificate_status;
    VpsChannelBindingStatus channel_binding_status;
    int ssl_in_use;
    int diagnostic_attributes_complete;
} VpsTlsOutcome;

VpsTlsResult vps_tls_policy_from_config(
    const VpsCredentialConfig *config,
    const VpsTlsPolicyOptions *options,
    VpsTlsPolicy *policy);
VpsTlsResult vps_tls_policy_verify(const VpsTlsPolicy *policy,
                                   const VpsTlsObservation *observation,
                                   VpsTlsOutcome *outcome,
                                   VpsLogger *logger);

const char *vps_tls_mode_name(VpsTlsMode mode);
const char *vps_channel_binding_mode_name(VpsChannelBindingMode mode);
const char *vps_tls_certificate_status_name(VpsTlsCertificateStatus status);
const char *vps_channel_binding_status_name(VpsChannelBindingStatus status);
const char *vps_tls_result_name(VpsTlsResult result);

#endif
