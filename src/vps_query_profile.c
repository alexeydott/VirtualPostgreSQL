#include "vps_query_profile.h"

#include <string.h>

#define VPS_PROFILE_HASH_OFFSET UINT64_C(1469598103934665603)
#define VPS_PROFILE_HASH_PRIME UINT64_C(1099511628211)

static int vps_profile_source_valid(VpsQueryProfileSource source)
{
    return source >= VPS_QUERY_PROFILE_HOST &&
           source <= VPS_QUERY_PROFILE_NAMED_REGISTRY;
}

static uint64_t vps_profile_name_hash(const char *name, size_t length)
{
    uint64_t hash = VPS_PROFILE_HASH_OFFSET;
    size_t index;
    for (index = 0U; index < length; ++index) {
        hash = (hash ^ (uint64_t)(unsigned char)name[index]) *
               VPS_PROFILE_HASH_PRIME;
    }
    return hash;
}

static int vps_profile_name_valid(const char *name, size_t length)
{
    size_t index;
    if (name == NULL || length == 0U ||
        length > VPS_QUERY_PROFILE_MAX_NAME_BYTES) return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)name[index];
        if (value == 0U || value < 0x20U || value == 0x7fU) return 0;
    }
    return 1;
}

static VpsQueryProfileResult vps_profile_lock(VpsQueryProfileRegistry *registry)
{
    return vps_platform_mutex_lock(registry->operations, &registry->mutex) ==
                   VPS_PLATFORM_OK
               ? VPS_QUERY_PROFILE_OK : VPS_QUERY_PROFILE_PLATFORM_ERROR;
}

static VpsQueryProfileResult vps_profile_unlock(
    VpsQueryProfileRegistry *registry)
{
    return vps_platform_mutex_unlock(registry->operations, &registry->mutex) ==
                   VPS_PLATFORM_OK
               ? VPS_QUERY_PROFILE_OK : VPS_QUERY_PROFILE_PLATFORM_ERROR;
}

static void vps_profile_log(VpsQueryProfileRegistry *registry,
                            VpsQueryProfileSource source,
                            uint64_t name_fingerprint,
                            uint64_t generation,
                            uint64_t profile_version,
                            const char *status,
                            VpsLogLevel level)
{
    VpsLogEvent event;
    const char *source_name = vps_query_profile_source_name(source);
    if (registry->logger == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK) return;
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION,
                                   "query_profile", 13U);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE,
                                   source_name, strlen(source_name));
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_QUERY_FINGERPRINT,
                                   name_fingerprint);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_GENERATION,
                                   generation);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_VERSION,
                                   profile_version);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS,
                                   status, strlen(status));
    vps_logger_emit(registry->logger, &event);
}

VpsQueryProfileResult vps_query_profile_registry_init(
    VpsQueryProfileRegistry *registry,
    const VpsPlatformOperations *operations,
    VpsLogger *logger)
{
    if (registry == NULL ||
        vps_platform_validate_operations(operations, VPS_PLATFORM_CAP_MUTEX) !=
            VPS_PLATFORM_OK) return VPS_QUERY_PROFILE_INVALID_ARGUMENT;
    (void)memset(registry, 0, sizeof(*registry));
    registry->operations = operations;
    registry->logger = logger;
    if (vps_platform_mutex_init(operations, &registry->mutex) !=
        VPS_PLATFORM_OK) return VPS_QUERY_PROFILE_PLATFORM_ERROR;
    registry->initialized = 1;
    return VPS_QUERY_PROFILE_OK;
}

VpsQueryProfileResult vps_query_profile_registry_register(
    VpsQueryProfileRegistry *registry,
    VpsQueryProfileSource source,
    uint64_t provider_id,
    const VpsQueryProfileProvider *provider)
{
    VpsQueryProfileProviderSlot *slot;
    VpsQueryProfileResult result;
    uint32_t minimum = (uint32_t)(offsetof(VpsQueryProfileProvider,
                                           provider_context) +
                                   sizeof(provider->provider_context));
    if (registry == NULL || !registry->initialized ||
        !vps_profile_source_valid(source) || provider_id == 0U ||
        provider == NULL || provider->resolve == NULL ||
        provider->release == NULL) {
        return VPS_QUERY_PROFILE_INVALID_ARGUMENT;
    }
    if (provider->structure_size < minimum ||
        provider->contract_version != VPS_QUERY_PROFILE_CONTRACT_VERSION) {
        return VPS_QUERY_PROFILE_ABI_INCOMPATIBLE;
    }
    result = vps_profile_lock(registry);
    if (result != VPS_QUERY_PROFILE_OK) return result;
    slot = &registry->slots[(size_t)source];
    if (slot->resolve_started) {
        (void)vps_profile_unlock(registry);
        return VPS_QUERY_PROFILE_REPLACEMENT_FORBIDDEN;
    }
    if (slot->generation == UINT64_MAX) {
        (void)vps_profile_unlock(registry);
        return VPS_QUERY_PROFILE_BUSY;
    }
    slot->provider = *provider;
    slot->provider_id = provider_id;
    ++slot->generation;
    slot->registered = 1;
    vps_profile_log(registry, source, 0U, slot->generation, 0U,
                    "registered", VPS_LOG_LEVEL_INFO);
    return vps_profile_unlock(registry);
}

VpsQueryProfileResult vps_resolved_query_profile_init(
    VpsResolvedQueryProfile *resolved,
    const VpsAllocator *allocator)
{
    if (resolved == NULL || !vps_allocator_is_valid(allocator)) {
        return VPS_QUERY_PROFILE_INVALID_ARGUMENT;
    }
    (void)memset(resolved, 0, sizeof(*resolved));
    if (vps_buffer_init(&resolved->query, allocator,
                        VPS_QUERY_SOURCE_MAX_BYTES + 1U) != VPS_MEMORY_OK) {
        return VPS_QUERY_PROFILE_INVALID_ARGUMENT;
    }
    resolved->initialized = 1;
    return VPS_QUERY_PROFILE_OK;
}

void vps_resolved_query_profile_cleanup(VpsResolvedQueryProfile *resolved)
{
    if (resolved == NULL || !resolved->initialized) return;
    vps_buffer_reset(&resolved->query);
    (void)memset(&resolved->analysis, 0, sizeof(resolved->analysis));
    resolved->provider_id = 0U;
    resolved->generation = 0U;
    resolved->profile_version = 0U;
    resolved->name_fingerprint = 0U;
    resolved->source = VPS_QUERY_PROFILE_HOST;
    resolved->initialized = 0;
}

static void vps_profile_resolve_finish(VpsQueryProfileRegistry *registry,
                                       VpsQueryProfileSource source,
                                       uint64_t name_fingerprint,
                                       uint64_t generation,
                                       uint64_t profile_version,
                                       VpsQueryProfileResult result)
{
    if (vps_profile_lock(registry) != VPS_QUERY_PROFILE_OK) return;
    if (registry->slots[(size_t)source].active_resolves != 0U) {
        --registry->slots[(size_t)source].active_resolves;
    }
    vps_profile_log(registry, source, name_fingerprint, generation,
                    profile_version, vps_query_profile_result_name(result),
                    result == VPS_QUERY_PROFILE_OK ? VPS_LOG_LEVEL_DEBUG
                                                   : VPS_LOG_LEVEL_WARN);
    (void)vps_profile_unlock(registry);
}

VpsQueryProfileResult vps_query_profile_registry_resolve(
    VpsQueryProfileRegistry *registry,
    VpsQueryProfileSource source,
    const char *profile_name,
    size_t profile_name_length,
    VpsResolvedQueryProfile *resolved)
{
    VpsQueryProfileProviderSlot *slot;
    VpsQueryProfileProvider provider;
    VpsQueryProfileLease lease;
    VpsQueryProfileResult result;
    uint64_t profile_version = 0U;
    uint64_t provider_id;
    uint64_t generation;
    uint64_t name_fingerprint;
    static const unsigned char terminator = 0U;

    if (registry == NULL || !registry->initialized ||
        !vps_profile_source_valid(source) ||
        !vps_profile_name_valid(profile_name, profile_name_length) ||
        resolved == NULL || !resolved->initialized ||
        resolved->query.data != NULL) return VPS_QUERY_PROFILE_INVALID_ARGUMENT;
    name_fingerprint = vps_profile_name_hash(profile_name, profile_name_length);
    result = vps_profile_lock(registry);
    if (result != VPS_QUERY_PROFILE_OK) return result;
    slot = &registry->slots[(size_t)source];
    if (!slot->registered) {
        (void)vps_profile_unlock(registry);
        return VPS_QUERY_PROFILE_NOT_REGISTERED;
    }
    if (slot->active_resolves == UINT64_MAX) {
        (void)vps_profile_unlock(registry);
        return VPS_QUERY_PROFILE_BUSY;
    }
    slot->resolve_started = 1;
    ++slot->active_resolves;
    provider = slot->provider;
    provider_id = slot->provider_id;
    generation = slot->generation;
    result = vps_profile_unlock(registry);
    if (result != VPS_QUERY_PROFILE_OK) return result;

    (void)memset(&lease, 0, sizeof(lease));
    lease.structure_size = (uint32_t)sizeof(lease);
    lease.contract_version = VPS_QUERY_PROFILE_CONTRACT_VERSION;
    result = provider.resolve(provider.provider_context, profile_name,
                              profile_name_length, &lease);
    if (result != VPS_QUERY_PROFILE_OK) {
        vps_profile_resolve_finish(registry, source, name_fingerprint,
                                   generation, 0U, result);
        return result;
    }
    if (lease.structure_size < sizeof(lease) ||
        lease.contract_version != VPS_QUERY_PROFILE_CONTRACT_VERSION ||
        lease.query == NULL || lease.query_length == 0U ||
        lease.query_length > VPS_QUERY_SOURCE_MAX_BYTES ||
        lease.profile_version == 0U) {
        result = VPS_QUERY_PROFILE_INVALID_LEASE;
    } else if (vps_buffer_append(&resolved->query, lease.query,
                                 lease.query_length) != VPS_MEMORY_OK ||
               vps_buffer_append(&resolved->query, &terminator, 1U) !=
                   VPS_MEMORY_OK) {
        result = VPS_QUERY_PROFILE_OUT_OF_MEMORY;
    } else if (vps_query_source_scan((const char *)resolved->query.data,
                                     lease.query_length, registry->logger,
                                     &resolved->analysis) !=
               VPS_QUERY_SOURCE_OK) {
        result = VPS_QUERY_PROFILE_INVALID_QUERY;
    }
    profile_version = lease.profile_version;
    provider.release(provider.provider_context, &lease);
    if (result == VPS_QUERY_PROFILE_OK) {
        resolved->source = source;
        resolved->provider_id = provider_id;
        resolved->generation = generation;
        resolved->profile_version = profile_version;
        resolved->name_fingerprint = name_fingerprint;
    } else {
        vps_buffer_reset(&resolved->query);
        (void)vps_buffer_init(&resolved->query, &resolved->query.allocator,
                              VPS_QUERY_SOURCE_MAX_BYTES + 1U);
    }
    vps_profile_resolve_finish(registry, source, name_fingerprint, generation,
                               profile_version, result);
    return result;
}

VpsQueryProfileResult vps_query_profile_registry_cleanup(
    VpsQueryProfileRegistry *registry)
{
    size_t index;
    VpsQueryProfileResult result;
    if (registry == NULL) return VPS_QUERY_PROFILE_INVALID_ARGUMENT;
    if (!registry->initialized) return VPS_QUERY_PROFILE_OK;
    result = vps_profile_lock(registry);
    if (result != VPS_QUERY_PROFILE_OK) return result;
    for (index = 0U; index < VPS_QUERY_PROFILE_PROVIDER_COUNT; ++index) {
        if (registry->slots[index].active_resolves != 0U) {
            (void)vps_profile_unlock(registry);
            return VPS_QUERY_PROFILE_BUSY;
        }
    }
    (void)memset(registry->slots, 0, sizeof(registry->slots));
    result = vps_profile_unlock(registry);
    if (result != VPS_QUERY_PROFILE_OK) return result;
    if (vps_platform_mutex_destroy(registry->operations, &registry->mutex) !=
        VPS_PLATFORM_OK) return VPS_QUERY_PROFILE_PLATFORM_ERROR;
    registry->initialized = 0;
    return VPS_QUERY_PROFILE_OK;
}

const char *vps_query_profile_result_name(VpsQueryProfileResult result)
{
    static const char *const names[] = {
        "ok", "invalid_argument", "abi_incompatible", "not_registered",
        "replacement_forbidden", "not_found", "invalid_lease",
        "invalid_query", "out_of_memory", "busy", "platform_error"
    };
    if (result < VPS_QUERY_PROFILE_OK ||
        result > VPS_QUERY_PROFILE_PLATFORM_ERROR) return "unknown";
    return names[(size_t)result];
}

const char *vps_query_profile_source_name(VpsQueryProfileSource source)
{
    switch (source) {
    case VPS_QUERY_PROFILE_HOST: return "host";
    case VPS_QUERY_PROFILE_PROTECTED_CONFIG: return "protected_config";
    case VPS_QUERY_PROFILE_ENVIRONMENT: return "environment";
    case VPS_QUERY_PROFILE_NAMED_REGISTRY: return "named_registry";
    default: return "unknown";
    }
}
