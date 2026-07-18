#include "vps_query_cache.h"

#include <string.h>

struct VpsQueryCache {
    VpsAllocator allocator;
    const VpsPlatformOperations *platform;
    VpsLogger *logger;
    VpsPlatformMutex mutex;
    VpsPlatformCondition condition;
    VpsEmbeddedSqlite *published;
    char *temp_path;
    size_t temp_path_size;
    VpsEmbeddedSqliteMode mode;
    VpsQueryCacheState state;
    uint64_t source_fingerprint;
    uint64_t layout_fingerprint;
    uint64_t generation;
    uint64_t build_attempts;
    uint64_t remote_executions;
    size_t leases;
    uint32_t wait_slice_ms;
    int mutex_initialized;
    int condition_initialized;
};

static void vps_query_cache_log(VpsQueryCache *cache,
                                VpsLogLevel level,
                                const char *phase,
                                const char *status)
{
    VpsLogEvent event;
    const char *mode;
    if (cache == NULL || cache->logger == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK) return;
    mode = cache->mode == VPS_EMBEDDED_SQLITE_TEMP ? "temp" : "memory";
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION,
                                   "query-cache", 11U);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                   strlen(phase));
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_SNAPSHOT_MODE,
                                   mode, strlen(mode));
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                   strlen(status));
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_GENERATION,
                                   cache->generation);
    (void)vps_log_event_add_uint64(&event,
                                   VPS_LOG_FIELD_QUERY_FINGERPRINT,
                                   cache->source_fingerprint);
    vps_logger_emit(cache->logger, &event);
}

VpsQueryCacheStatus vps_query_cache_create(const VpsQueryCacheConfig *config,
                                           VpsQueryCache **cache)
{
    VpsQueryCache *candidate = NULL;
    size_t path_size;
    if (config == NULL || cache == NULL || *cache != NULL ||
        !vps_allocator_is_valid(&config->allocator) ||
        config->platform == NULL || config->wait_slice_ms == 0U ||
        (config->mode != VPS_EMBEDDED_SQLITE_MEMORY &&
         config->mode != VPS_EMBEDDED_SQLITE_TEMP) ||
        (config->mode == VPS_EMBEDDED_SQLITE_TEMP &&
         (config->temp_path == NULL || config->temp_path_length == 0U)))
        return VPS_QUERY_CACHE_INVALID_ARGUMENT;
    if (vps_memory_allocate(&config->allocator, sizeof(*candidate),
                            (void **)&candidate) != VPS_MEMORY_OK)
        return VPS_QUERY_CACHE_OUT_OF_MEMORY;
    (void)memset(candidate, 0, sizeof(*candidate));
    candidate->allocator = config->allocator;
    candidate->platform = config->platform;
    candidate->logger = config->logger;
    candidate->mode = config->mode;
    candidate->source_fingerprint = config->source_fingerprint;
    candidate->layout_fingerprint = config->layout_fingerprint;
    candidate->wait_slice_ms = config->wait_slice_ms;
    candidate->state = VPS_QUERY_CACHE_EMPTY;
    if (config->mode == VPS_EMBEDDED_SQLITE_TEMP) {
        if (vps_size_add(config->temp_path_length, 1U, &path_size) !=
                VPS_MEMORY_OK ||
            vps_memory_allocate(&candidate->allocator, path_size,
                                (void **)&candidate->temp_path) != VPS_MEMORY_OK)
            goto memory_error;
        (void)memcpy(candidate->temp_path, config->temp_path,
                     config->temp_path_length);
        candidate->temp_path[config->temp_path_length] = '\0';
        candidate->temp_path_size = path_size;
    }
    if (vps_platform_mutex_init(candidate->platform, &candidate->mutex) !=
        VPS_PLATFORM_OK) goto platform_error;
    candidate->mutex_initialized = 1;
    if (vps_platform_condition_init(candidate->platform,
                                    &candidate->condition) != VPS_PLATFORM_OK)
        goto platform_error;
    candidate->condition_initialized = 1;
    *cache = candidate;
    return VPS_QUERY_CACHE_OK;
memory_error:
    vps_memory_release(&candidate->allocator, (void **)&candidate,
                       sizeof(*candidate));
    return VPS_QUERY_CACHE_OUT_OF_MEMORY;
platform_error:
    if (candidate->mutex_initialized)
        (void)vps_platform_mutex_destroy(candidate->platform,
                                         &candidate->mutex);
    vps_memory_release(&candidate->allocator, (void **)&candidate->temp_path,
                       candidate->temp_path_size);
    vps_memory_release(&candidate->allocator, (void **)&candidate,
                       sizeof(*candidate));
    return VPS_QUERY_CACHE_PLATFORM_ERROR;
}

static VpsQueryCacheStatus vps_query_cache_wait(VpsQueryCache *cache,
                                                uint64_t timeout_ms)
{
    uint64_t start = 0U;
    uint64_t now = 0U;
    if (vps_platform_monotonic_now_ms(cache->platform, &start) !=
        VPS_PLATFORM_OK) return VPS_QUERY_CACHE_PLATFORM_ERROR;
    while (cache->state == VPS_QUERY_CACHE_BUILDING) {
        uint64_t elapsed;
        uint64_t remaining;
        uint32_t slice;
        VpsPlatformStatus wait_status;
        if (vps_platform_monotonic_now_ms(cache->platform, &now) !=
            VPS_PLATFORM_OK) return VPS_QUERY_CACHE_PLATFORM_ERROR;
        elapsed = now >= start ? now - start : 0U;
        if (elapsed >= timeout_ms) return VPS_QUERY_CACHE_TIMEOUT;
        remaining = timeout_ms - elapsed;
        slice = remaining < cache->wait_slice_ms ? (uint32_t)remaining
                                                 : cache->wait_slice_ms;
        if (slice == 0U) slice = 1U;
        wait_status = vps_platform_condition_wait(
            cache->platform, &cache->condition, &cache->mutex, slice);
        if (wait_status != VPS_PLATFORM_OK &&
            wait_status != VPS_PLATFORM_TIMEOUT)
            return VPS_QUERY_CACHE_PLATFORM_ERROR;
    }
    return VPS_QUERY_CACHE_OK;
}

VpsQueryCacheStatus vps_query_cache_acquire(
    VpsQueryCache *cache,
    uint64_t timeout_ms,
    VpsQueryCacheBuildFunction build,
    void *build_context,
    VpsQueryCacheLease *lease)
{
    VpsEmbeddedSqlite *candidate = NULL;
    VpsEmbeddedSqliteOpenOptions open_options;
    VpsQueryCacheStatus status;
    if (cache == NULL || timeout_ms == 0U || build == NULL || lease == NULL ||
        lease->owner != NULL || lease->database != NULL)
        return VPS_QUERY_CACHE_INVALID_ARGUMENT;
    if (vps_platform_mutex_lock(cache->platform, &cache->mutex) !=
        VPS_PLATFORM_OK) return VPS_QUERY_CACHE_PLATFORM_ERROR;
    status = vps_query_cache_wait(cache, timeout_ms);
    if (status != VPS_QUERY_CACHE_OK) {
        (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
        return status;
    }
    if (cache->state == VPS_QUERY_CACHE_READY) {
        cache->leases += 1U;
        lease->owner = cache;
        lease->database = cache->published;
        lease->generation = cache->generation;
        (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
        return VPS_QUERY_CACHE_OK;
    }
    if (cache->state != VPS_QUERY_CACHE_EMPTY) {
        (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
        return VPS_QUERY_CACHE_BUSY;
    }
    cache->state = VPS_QUERY_CACHE_BUILDING;
    cache->build_attempts += 1U;
    vps_query_cache_log(cache, VPS_LOG_LEVEL_INFO, "build", "started");
    (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);

    (void)memset(&open_options, 0, sizeof(open_options));
    open_options.allocator = cache->allocator;
    open_options.logger = cache->logger;
    open_options.mode = cache->mode;
    open_options.temp_path = cache->temp_path;
    open_options.temp_path_length = cache->temp_path_size == 0U
                                        ? 0U : cache->temp_path_size - 1U;
    status = vps_embedded_sqlite_open(&open_options, &candidate) ==
                     VPS_EMBEDDED_SQLITE_OK
                 ? build(build_context, candidate)
                 : VPS_QUERY_CACHE_BUILD_FAILED;
    if (status == VPS_QUERY_CACHE_OK &&
        vps_embedded_sqlite_seal(candidate) != VPS_EMBEDDED_SQLITE_OK)
        status = VPS_QUERY_CACHE_BUILD_FAILED;

    if (vps_platform_mutex_lock(cache->platform, &cache->mutex) !=
        VPS_PLATFORM_OK) {
        (void)vps_embedded_sqlite_close(&candidate);
        return VPS_QUERY_CACHE_PLATFORM_ERROR;
    }
    if (status == VPS_QUERY_CACHE_OK) {
        cache->published = candidate;
        cache->generation += 1U;
        cache->state = VPS_QUERY_CACHE_READY;
        cache->leases += 1U;
        lease->owner = cache;
        lease->database = candidate;
        lease->generation = cache->generation;
        vps_query_cache_log(cache, VPS_LOG_LEVEL_INFO, "publish", "ready");
    } else {
        cache->state = VPS_QUERY_CACHE_EMPTY;
        vps_query_cache_log(cache, VPS_LOG_LEVEL_WARN, "publish", "failed");
    }
    (void)vps_platform_condition_broadcast(cache->platform,
                                           &cache->condition);
    (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
    if (status != VPS_QUERY_CACHE_OK)
        (void)vps_embedded_sqlite_close(&candidate);
    return status;
}

VpsQueryCacheStatus vps_query_cache_lease_release(VpsQueryCacheLease *lease)
{
    VpsQueryCache *cache;
    if (lease == NULL || lease->owner == NULL || lease->database == NULL)
        return VPS_QUERY_CACHE_INVALID_ARGUMENT;
    cache = lease->owner;
    if (vps_platform_mutex_lock(cache->platform, &cache->mutex) !=
        VPS_PLATFORM_OK) return VPS_QUERY_CACHE_PLATFORM_ERROR;
    if (cache->leases == 0U || lease->database != cache->published) {
        (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
        return VPS_QUERY_CACHE_INVALID_ARGUMENT;
    }
    cache->leases -= 1U;
    (void)memset(lease, 0, sizeof(*lease));
    (void)vps_platform_condition_broadcast(cache->platform,
                                           &cache->condition);
    (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
    return VPS_QUERY_CACHE_OK;
}

VpsQueryCacheStatus vps_query_cache_destroy(VpsQueryCache **cache)
{
    VpsQueryCache *owned;
    if (cache == NULL) return VPS_QUERY_CACHE_INVALID_ARGUMENT;
    owned = *cache;
    if (owned == NULL) return VPS_QUERY_CACHE_OK;
    if (vps_platform_mutex_lock(owned->platform, &owned->mutex) !=
        VPS_PLATFORM_OK) return VPS_QUERY_CACHE_PLATFORM_ERROR;
    if (owned->state == VPS_QUERY_CACHE_BUILDING || owned->leases != 0U) {
        (void)vps_platform_mutex_unlock(owned->platform, &owned->mutex);
        return VPS_QUERY_CACHE_BUSY;
    }
    owned->state = VPS_QUERY_CACHE_CLOSING;
    (void)vps_platform_mutex_unlock(owned->platform, &owned->mutex);
    (void)vps_embedded_sqlite_close(&owned->published);
    if (owned->condition_initialized)
        (void)vps_platform_condition_destroy(owned->platform,
                                             &owned->condition);
    if (owned->mutex_initialized)
        (void)vps_platform_mutex_destroy(owned->platform, &owned->mutex);
    vps_memory_release(&owned->allocator, (void **)&owned->temp_path,
                       owned->temp_path_size);
    *cache = NULL;
    vps_memory_release(&owned->allocator, (void **)&owned, sizeof(*owned));
    return VPS_QUERY_CACHE_OK;
}

VpsQueryCacheState vps_query_cache_state(VpsQueryCache *cache)
{
    VpsQueryCacheState state = VPS_QUERY_CACHE_CLOSING;
    if (cache != NULL &&
        vps_platform_mutex_lock(cache->platform, &cache->mutex) ==
            VPS_PLATFORM_OK) {
        state = cache->state;
        (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
    }
    return state;
}

uint64_t vps_query_cache_generation(VpsQueryCache *cache)
{
    uint64_t value = 0U;
    if (cache != NULL &&
        vps_platform_mutex_lock(cache->platform, &cache->mutex) ==
            VPS_PLATFORM_OK) {
        value = cache->generation;
        (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
    }
    return value;
}

uint64_t vps_query_cache_build_attempts(VpsQueryCache *cache)
{
    uint64_t value = 0U;
    if (cache != NULL &&
        vps_platform_mutex_lock(cache->platform, &cache->mutex) ==
            VPS_PLATFORM_OK) {
        value = cache->build_attempts;
        (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
    }
    return value;
}

uint64_t vps_query_cache_remote_executions(VpsQueryCache *cache)
{
    uint64_t value = 0U;
    if (cache != NULL &&
        vps_platform_mutex_lock(cache->platform, &cache->mutex) ==
            VPS_PLATFORM_OK) {
        value = cache->remote_executions;
        (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
    }
    return value;
}

void vps_query_cache_record_remote_execution(VpsQueryCache *cache)
{
    if (cache != NULL &&
        vps_platform_mutex_lock(cache->platform, &cache->mutex) ==
            VPS_PLATFORM_OK) {
        if (cache->remote_executions != UINT64_MAX)
            cache->remote_executions += 1U;
        (void)vps_platform_mutex_unlock(cache->platform, &cache->mutex);
    }
}

const char *vps_query_cache_status_name(VpsQueryCacheStatus status)
{
    switch (status) {
        case VPS_QUERY_CACHE_OK: return "ok";
        case VPS_QUERY_CACHE_INVALID_ARGUMENT: return "invalid_argument";
        case VPS_QUERY_CACHE_OUT_OF_MEMORY: return "out_of_memory";
        case VPS_QUERY_CACHE_BUSY: return "busy";
        case VPS_QUERY_CACHE_TIMEOUT: return "timeout";
        case VPS_QUERY_CACHE_BUILD_FAILED: return "build_failed";
        case VPS_QUERY_CACHE_PLATFORM_ERROR: return "platform_error";
        default: return "unknown";
    }
}

const char *vps_query_cache_state_name(VpsQueryCacheState state)
{
    switch (state) {
        case VPS_QUERY_CACHE_EMPTY: return "empty";
        case VPS_QUERY_CACHE_BUILDING: return "building";
        case VPS_QUERY_CACHE_READY: return "ready";
        case VPS_QUERY_CACHE_CLOSING: return "closing";
        default: return "unknown";
    }
}
