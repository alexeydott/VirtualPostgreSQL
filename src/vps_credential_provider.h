#ifndef VPS_CREDENTIAL_PROVIDER_H
#define VPS_CREDENTIAL_PROVIDER_H

#include "virtualpostgresql/vps_api.h"
#include "vps_logging.h"
#include "vps_memory.h"
#include "vps_platform.h"
#include "vps_secure_memory.h"

#include <stdint.h>

typedef enum VpsCredentialRegistryResult {
    VPS_CREDENTIAL_REGISTRY_OK = 0,
    VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT = 1,
    VPS_CREDENTIAL_REGISTRY_ABI_INCOMPATIBLE = 2,
    VPS_CREDENTIAL_REGISTRY_NOT_REGISTERED = 3,
    VPS_CREDENTIAL_REGISTRY_REPLACEMENT_FORBIDDEN = 4,
    VPS_CREDENTIAL_REGISTRY_RESOLVE_FAILED = 5,
    VPS_CREDENTIAL_REGISTRY_INVALID_CONFIG = 6,
    VPS_CREDENTIAL_REGISTRY_OUT_OF_MEMORY = 7,
    VPS_CREDENTIAL_REGISTRY_BUSY = 8,
    VPS_CREDENTIAL_REGISTRY_PLATFORM_ERROR = 9,
    VPS_CREDENTIAL_REGISTRY_CLEANUP_FAILED = 10
} VpsCredentialRegistryResult;

/*
 * Registry copies the provider dispatch table but borrows provider_context,
 * platform operations and logger. Callers must keep all three alive until a
 * successful registry cleanup. Resolve callbacks may run concurrently.
 */
typedef struct VpsCredentialRegistry {
    VpsPlatformMutex mutex;
    const VpsPlatformOperations *operations;
    VpsLogger *logger;
    VpsCredentialProvider provider;
    uint64_t provider_id;
    uint64_t generation;
    uint64_t active_resolves;
    int initialized;
    int registered;
    int resolve_started;
} VpsCredentialRegistry;

/*
 * A resolved value owns one securely-erased allocation. Config pointers refer
 * into that allocation and remain valid until cleanup. Values are not shared
 * between concurrent resolves and cleanup is idempotent.
 */
typedef struct VpsResolvedCredential {
    VpsCredentialConfig config;
    VpsSensitiveMemory storage;
    uint64_t provider_id;
    uint64_t generation;
    int initialized;
} VpsResolvedCredential;

VpsCredentialRegistryResult vps_credential_registry_init(
    VpsCredentialRegistry *registry,
    const VpsPlatformOperations *operations,
    VpsLogger *logger);
VpsCredentialRegistryResult vps_credential_registry_register(
    VpsCredentialRegistry *registry,
    uint64_t provider_id,
    const VpsCredentialProvider *provider);
VpsCredentialRegistryResult vps_credential_registry_resolve(
    VpsCredentialRegistry *registry,
    const char *credential_ref,
    uint32_t credential_ref_length,
    VpsResolvedCredential *resolved);
VpsCredentialRegistryResult vps_credential_registry_cleanup(
    VpsCredentialRegistry *registry);

VpsCredentialRegistryResult vps_resolved_credential_init(
    VpsResolvedCredential *resolved,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger);
VpsCredentialRegistryResult vps_resolved_credential_cleanup(
    VpsResolvedCredential *resolved);

const char *vps_credential_registry_result_name(
    VpsCredentialRegistryResult result);

#endif
