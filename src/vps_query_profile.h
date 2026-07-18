#ifndef VPS_QUERY_PROFILE_H
#define VPS_QUERY_PROFILE_H

#include "vps_memory.h"
#include "vps_platform.h"
#include "vps_query_source.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_QUERY_PROFILE_CONTRACT_VERSION UINT32_C(1)
#define VPS_QUERY_PROFILE_MAX_NAME_BYTES 255U
#define VPS_QUERY_PROFILE_PROVIDER_COUNT 4U

typedef enum VpsQueryProfileSource {
    VPS_QUERY_PROFILE_HOST = 0,
    VPS_QUERY_PROFILE_PROTECTED_CONFIG = 1,
    VPS_QUERY_PROFILE_ENVIRONMENT = 2,
    VPS_QUERY_PROFILE_NAMED_REGISTRY = 3
} VpsQueryProfileSource;

typedef enum VpsQueryProfileResult {
    VPS_QUERY_PROFILE_OK = 0,
    VPS_QUERY_PROFILE_INVALID_ARGUMENT = 1,
    VPS_QUERY_PROFILE_ABI_INCOMPATIBLE = 2,
    VPS_QUERY_PROFILE_NOT_REGISTERED = 3,
    VPS_QUERY_PROFILE_REPLACEMENT_FORBIDDEN = 4,
    VPS_QUERY_PROFILE_NOT_FOUND = 5,
    VPS_QUERY_PROFILE_INVALID_LEASE = 6,
    VPS_QUERY_PROFILE_INVALID_QUERY = 7,
    VPS_QUERY_PROFILE_OUT_OF_MEMORY = 8,
    VPS_QUERY_PROFILE_BUSY = 9,
    VPS_QUERY_PROFILE_PLATFORM_ERROR = 10
} VpsQueryProfileResult;

typedef struct VpsQueryProfileLease {
    uint32_t structure_size;
    uint32_t contract_version;
    const char *query;
    size_t query_length;
    uint64_t profile_version;
    void *lease_context;
} VpsQueryProfileLease;

typedef VpsQueryProfileResult (*VpsQueryProfileResolveFunction)(
    void *provider_context,
    const char *profile_name,
    size_t profile_name_length,
    VpsQueryProfileLease *lease);
typedef void (*VpsQueryProfileReleaseFunction)(
    void *provider_context,
    VpsQueryProfileLease *lease);

typedef struct VpsQueryProfileProvider {
    uint32_t structure_size;
    uint32_t contract_version;
    VpsQueryProfileResolveFunction resolve;
    VpsQueryProfileReleaseFunction release;
    void *provider_context;
} VpsQueryProfileProvider;

typedef struct VpsQueryProfileProviderSlot {
    VpsQueryProfileProvider provider;
    uint64_t provider_id;
    uint64_t generation;
    uint64_t active_resolves;
    int registered;
    int resolve_started;
} VpsQueryProfileProviderSlot;

/* Registry borrows platform operations/logger and provider contexts. */
typedef struct VpsQueryProfileRegistry {
    VpsPlatformMutex mutex;
    const VpsPlatformOperations *operations;
    VpsLogger *logger;
    VpsQueryProfileProviderSlot slots[VPS_QUERY_PROFILE_PROVIDER_COUNT];
    int initialized;
} VpsQueryProfileRegistry;

/* Resolved profile exclusively owns query bytes until cleanup. */
typedef struct VpsResolvedQueryProfile {
    VpsBuffer query;
    VpsQuerySourceAnalysis analysis;
    VpsQueryProfileSource source;
    uint64_t provider_id;
    uint64_t generation;
    uint64_t profile_version;
    uint64_t name_fingerprint;
    int initialized;
} VpsResolvedQueryProfile;

VpsQueryProfileResult vps_query_profile_registry_init(
    VpsQueryProfileRegistry *registry,
    const VpsPlatformOperations *operations,
    VpsLogger *logger);
VpsQueryProfileResult vps_query_profile_registry_register(
    VpsQueryProfileRegistry *registry,
    VpsQueryProfileSource source,
    uint64_t provider_id,
    const VpsQueryProfileProvider *provider);
VpsQueryProfileResult vps_query_profile_registry_resolve(
    VpsQueryProfileRegistry *registry,
    VpsQueryProfileSource source,
    const char *profile_name,
    size_t profile_name_length,
    VpsResolvedQueryProfile *resolved);
VpsQueryProfileResult vps_query_profile_registry_cleanup(
    VpsQueryProfileRegistry *registry);

VpsQueryProfileResult vps_resolved_query_profile_init(
    VpsResolvedQueryProfile *resolved,
    const VpsAllocator *allocator);
void vps_resolved_query_profile_cleanup(VpsResolvedQueryProfile *resolved);

const char *vps_query_profile_result_name(VpsQueryProfileResult result);
const char *vps_query_profile_source_name(VpsQueryProfileSource source);

#endif
