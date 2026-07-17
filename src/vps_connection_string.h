#ifndef VPS_CONNECTION_STRING_H
#define VPS_CONNECTION_STRING_H

#include "vps_arguments.h"
#include "vps_credential_provider.h"

#define VPS_PROFILE_NAME_MAX_LENGTH 64U

typedef enum VpsConnectionMode {
    VPS_CONNECTION_MODE_NONE = 0,
    VPS_CONNECTION_MODE_CREDENTIAL_REF = 1,
    VPS_CONNECTION_MODE_SERVICE = 2,
    VPS_CONNECTION_MODE_PROFILE = 3,
    VPS_CONNECTION_MODE_CONNSTR = 4
} VpsConnectionMode;

typedef enum VpsConnectionStringResult {
    VPS_CONNECTION_STRING_OK = 0,
    VPS_CONNECTION_STRING_INVALID_ARGUMENT = 1,
    VPS_CONNECTION_STRING_INVALID_MODE = 2,
    VPS_CONNECTION_STRING_INVALID_VALUE = 3,
    VPS_CONNECTION_STRING_LIMIT_EXCEEDED = 4,
    VPS_CONNECTION_STRING_PROVIDER_UNAVAILABLE = 5,
    VPS_CONNECTION_STRING_RESOLVE_FAILED = 6,
    VPS_CONNECTION_STRING_ENVIRONMENT_ERROR = 7,
    VPS_CONNECTION_STRING_CONNINFO_REJECTED = 8,
    VPS_CONNECTION_STRING_OUT_OF_MEMORY = 9,
    VPS_CONNECTION_STRING_CLEANUP_FAILED = 10
} VpsConnectionStringResult;

typedef VpsConnectionStringResult (*VpsConninfoConsumer)(
    void *context,
    const VpsCredentialConfig *config);

typedef VpsConnectionStringResult (*VpsConninfoParseFunction)(
    void *context,
    const char *conninfo,
    size_t conninfo_length,
    VpsConninfoConsumer consumer,
    void *consumer_context);

typedef struct VpsConninfoParser {
    void *context;
    VpsConninfoParseFunction parse;
} VpsConninfoParser;

typedef struct VpsProfileEnvironmentEntry {
    const char *name;
    const char *value;
} VpsProfileEnvironmentEntry;

typedef struct VpsConnectionResolveOptions {
    VpsCredentialRegistry *credential_registry;
    const VpsConninfoParser *conninfo_parser;
    const VpsProfileEnvironmentEntry *profile_environment;
    size_t profile_environment_count;
} VpsConnectionResolveOptions;

/*
 * Config pointers refer into secure storage and remain valid until cleanup or
 * the next successful resolve. Resolve is transactional. The optional
 * profile_environment snapshot exists for deterministic hosts/tests and is
 * validated for duplicate and unknown profile fields; otherwise known fields
 * are read through the platform environment abstraction.
 */
typedef struct VpsConnectionConfig {
    VpsCredentialConfig config;
    VpsSensitiveMemory storage;
    VpsConnectionMode mode;
    uint64_t provider_id;
    uint64_t generation;
    char normalized_profile[VPS_PROFILE_NAME_MAX_LENGTH + 1U];
    int persistent_connstr_risk;
    int initialized;
} VpsConnectionConfig;

VpsConnectionStringResult vps_connection_config_init(
    VpsConnectionConfig *connection,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger);
VpsConnectionStringResult vps_connection_config_resolve(
    VpsConnectionConfig *connection,
    const VpsParsedArguments *arguments,
    const VpsConnectionResolveOptions *options);
VpsConnectionStringResult vps_connection_config_cleanup(
    VpsConnectionConfig *connection);

const char *vps_connection_mode_name(VpsConnectionMode mode);
const char *vps_connection_string_result_name(VpsConnectionStringResult result);

#endif
