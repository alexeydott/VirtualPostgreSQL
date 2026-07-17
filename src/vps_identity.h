#ifndef VPS_IDENTITY_H
#define VPS_IDENTITY_H

#include "vps_connection_string.h"

#define VPS_IDENTITY_CANONICAL_LIMIT 65536U
#define VPS_IDENTITY_FINGERPRINT_LENGTH 64U
#define VPS_IDENTITY_FINGERPRINT_BUFFER_SIZE 65U

typedef enum VpsIdentityResult {
    VPS_IDENTITY_OK = 0,
    VPS_IDENTITY_INVALID_ARGUMENT = 1,
    VPS_IDENTITY_INVALID_VALUE = 2,
    VPS_IDENTITY_LIMIT_EXCEEDED = 3,
    VPS_IDENTITY_OUT_OF_MEMORY = 4
} VpsIdentityResult;

typedef enum VpsIdentityComparison {
    VPS_IDENTITY_DIFFERENT = 0,
    VPS_IDENTITY_SAME = 1,
    VPS_IDENTITY_GENERATION_CHANGED = 2
} VpsIdentityComparison;

typedef struct VpsIdentityBuildOptions {
    const char *backend_name;
    size_t backend_name_length;
    uint64_t credential_generation;
    uint64_t configuration_generation;
} VpsIdentityBuildOptions;

/*
 * canonical owns a versioned, type-tagged, length-prefixed byte sequence.
 * It never contains passwords, tokens, raw conninfo, provider references or
 * lease data. Exact comparisons use canonical bytes, not the diagnostic hash.
 * The allocator and logger are borrowed; mutation is not thread-safe.
 */
typedef struct VpsConnectionIdentity {
    VpsBuffer canonical;
    VpsLogger *logger;
    uint64_t credential_generation;
    uint64_t configuration_generation;
    char fingerprint[VPS_IDENTITY_FINGERPRINT_BUFFER_SIZE];
    int initialized;
    int built;
} VpsConnectionIdentity;

VpsIdentityResult vps_identity_init(VpsConnectionIdentity *identity,
                                    const VpsAllocator *allocator,
                                    VpsLogger *logger);
VpsIdentityResult vps_identity_build(
    VpsConnectionIdentity *identity,
    const VpsConnectionConfig *connection,
    const VpsParsedArguments *arguments,
    const VpsIdentityBuildOptions *options);
void vps_identity_cleanup(VpsConnectionIdentity *identity);

VpsIdentityComparison vps_identity_compare(
    const VpsConnectionIdentity *left,
    const VpsConnectionIdentity *right);
const char *vps_identity_fingerprint(const VpsConnectionIdentity *identity);
const char *vps_identity_result_name(VpsIdentityResult result);
const char *vps_identity_comparison_name(VpsIdentityComparison comparison);

#endif
