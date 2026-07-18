#ifndef VPS_QUERY_CACHE_H
#define VPS_QUERY_CACHE_H

#include "vps_embedded_sqlite.h"
#include "vps_platform.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_QUERY_CACHE_CONTRACT_VERSION UINT32_C(1)
#define VPS_QUERY_CACHE_DEFAULT_WAIT_SLICE_MS UINT32_C(50)

typedef enum VpsQueryCacheStatus {
    VPS_QUERY_CACHE_OK = 0,
    VPS_QUERY_CACHE_INVALID_ARGUMENT = 1,
    VPS_QUERY_CACHE_OUT_OF_MEMORY = 2,
    VPS_QUERY_CACHE_BUSY = 3,
    VPS_QUERY_CACHE_TIMEOUT = 4,
    VPS_QUERY_CACHE_BUILD_FAILED = 5,
    VPS_QUERY_CACHE_PLATFORM_ERROR = 6
} VpsQueryCacheStatus;

typedef enum VpsQueryCacheState {
    VPS_QUERY_CACHE_EMPTY = 0,
    VPS_QUERY_CACHE_BUILDING = 1,
    VPS_QUERY_CACHE_READY = 2,
    VPS_QUERY_CACHE_CLOSING = 3
} VpsQueryCacheState;

typedef struct VpsQueryCache VpsQueryCache;

typedef struct VpsQueryCacheConfig {
    VpsAllocator allocator;
    const VpsPlatformOperations *platform;
    VpsLogger *logger;
    VpsEmbeddedSqliteMode mode;
    uint64_t source_fingerprint;
    uint64_t layout_fingerprint;
    const char *temp_path;
    size_t temp_path_length;
    uint32_t wait_slice_ms;
} VpsQueryCacheConfig;

typedef struct VpsQueryCacheLease {
    VpsQueryCache *owner;
    VpsEmbeddedSqlite *database;
    uint64_t generation;
} VpsQueryCacheLease;

typedef VpsQueryCacheStatus (*VpsQueryCacheBuildFunction)(
    void *context,
    VpsEmbeddedSqlite *candidate);

VpsQueryCacheStatus vps_query_cache_create(const VpsQueryCacheConfig *config,
                                           VpsQueryCache **cache);
VpsQueryCacheStatus vps_query_cache_acquire(
    VpsQueryCache *cache,
    uint64_t timeout_ms,
    VpsQueryCacheBuildFunction build,
    void *build_context,
    VpsQueryCacheLease *lease);
VpsQueryCacheStatus vps_query_cache_lease_release(VpsQueryCacheLease *lease);
VpsQueryCacheStatus vps_query_cache_destroy(VpsQueryCache **cache);

VpsQueryCacheState vps_query_cache_state(VpsQueryCache *cache);
uint64_t vps_query_cache_generation(VpsQueryCache *cache);
uint64_t vps_query_cache_build_attempts(VpsQueryCache *cache);
uint64_t vps_query_cache_remote_executions(VpsQueryCache *cache);
void vps_query_cache_record_remote_execution(VpsQueryCache *cache);
const char *vps_query_cache_status_name(VpsQueryCacheStatus status);
const char *vps_query_cache_state_name(VpsQueryCacheState state);

#endif
