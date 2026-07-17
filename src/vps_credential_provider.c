#include "vps_credential_provider.h"

#include "vps_private_api.h"

#include <stddef.h>
#include <string.h>

typedef struct VpsCredentialFieldDescriptor {
    VpsCredentialFields bit;
    size_t offset;
} VpsCredentialFieldDescriptor;

#define VPS_CREDENTIAL_MEMBER_OFFSET(member) \
    offsetof(VpsCredentialConfig, member)

static const VpsCredentialFieldDescriptor vps_credential_fields[] = {
    {VPS_CREDENTIAL_FIELD_HOSTS, VPS_CREDENTIAL_MEMBER_OFFSET(hosts)},
    {VPS_CREDENTIAL_FIELD_PORTS, VPS_CREDENTIAL_MEMBER_OFFSET(ports)},
    {VPS_CREDENTIAL_FIELD_USER, VPS_CREDENTIAL_MEMBER_OFFSET(user)},
    {VPS_CREDENTIAL_FIELD_PASSWORD, VPS_CREDENTIAL_MEMBER_OFFSET(password)},
    {VPS_CREDENTIAL_FIELD_DBNAME, VPS_CREDENTIAL_MEMBER_OFFSET(dbname)},
    {VPS_CREDENTIAL_FIELD_SERVICE, VPS_CREDENTIAL_MEMBER_OFFSET(service)},
    {VPS_CREDENTIAL_FIELD_SERVICE_FILE,
     VPS_CREDENTIAL_MEMBER_OFFSET(service_file)},
    {VPS_CREDENTIAL_FIELD_SSLMODE, VPS_CREDENTIAL_MEMBER_OFFSET(sslmode)},
    {VPS_CREDENTIAL_FIELD_SSLROOTCERT,
     VPS_CREDENTIAL_MEMBER_OFFSET(sslrootcert)},
    {VPS_CREDENTIAL_FIELD_SSLCERT, VPS_CREDENTIAL_MEMBER_OFFSET(sslcert)},
    {VPS_CREDENTIAL_FIELD_SSLKEY, VPS_CREDENTIAL_MEMBER_OFFSET(sslkey)},
    {VPS_CREDENTIAL_FIELD_SSLCRL, VPS_CREDENTIAL_MEMBER_OFFSET(sslcrl)},
    {VPS_CREDENTIAL_FIELD_CHANNEL_BINDING,
     VPS_CREDENTIAL_MEMBER_OFFSET(channel_binding)},
    {VPS_CREDENTIAL_FIELD_TARGET_SESSION_ATTRS,
     VPS_CREDENTIAL_MEMBER_OFFSET(target_session_attrs)},
    {VPS_CREDENTIAL_FIELD_CONNECT_TIMEOUT,
     VPS_CREDENTIAL_MEMBER_OFFSET(connect_timeout)},
    {VPS_CREDENTIAL_FIELD_STATEMENT_TIMEOUT,
     VPS_CREDENTIAL_MEMBER_OFFSET(statement_timeout)},
    {VPS_CREDENTIAL_FIELD_LOCK_TIMEOUT,
     VPS_CREDENTIAL_MEMBER_OFFSET(lock_timeout)},
    {VPS_CREDENTIAL_FIELD_APPLICATION_NAME,
     VPS_CREDENTIAL_MEMBER_OFFSET(application_name)},
    {VPS_CREDENTIAL_FIELD_SEARCH_PATH,
     VPS_CREDENTIAL_MEMBER_OFFSET(search_path)}};

static int vps_structure_has_member(uint32_t structure_size, size_t offset)
{
    return offset <= UINT32_MAX - sizeof(const char *) &&
           structure_size >= (uint32_t)(offset + sizeof(const char *));
}

static const char *vps_config_get_string(const VpsCredentialConfig *config,
                                         size_t offset)
{
    const char *const *member;

    if (!vps_structure_has_member(config->header.structure_size, offset)) {
        return NULL;
    }
    member = (const char *const *)((const unsigned char *)config + offset);
    return *member;
}

static void vps_config_set_string(VpsCredentialConfig *config,
                                  size_t offset,
                                  const char *value)
{
    const char **member =
        (const char **)((unsigned char *)config + offset);
    *member = value;
}

static int vps_utf8_is_valid(const char *value,
                             size_t length,
                             int reject_controls)
{
    size_t index = 0U;

    while (index < length) {
        unsigned char first = (unsigned char)value[index];
        size_t continuation_count;
        uint32_t codepoint;
        size_t continuation;

        if (first < 0x80U) {
            if (first == 0U ||
                (reject_controls && (first < 0x20U || first == 0x7fU))) {
                return 0;
            }
            index += 1U;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1U;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2U;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3U;
            codepoint = first & 0x07U;
        } else {
            return 0;
        }
        if (continuation_count > length - index - 1U) {
            return 0;
        }
        for (continuation = 1U; continuation <= continuation_count;
             ++continuation) {
            unsigned char current = (unsigned char)value[index + continuation];
            if ((current & 0xc0U) != 0x80U) {
                return 0;
            }
            codepoint = (codepoint << 6) | (current & 0x3fU);
        }
        if ((continuation_count == 2U && codepoint < 0x800U) ||
            (continuation_count == 3U && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return 0;
        }
        index += continuation_count + 1U;
    }
    return 1;
}

static int vps_bounded_string_length(const char *value, size_t *length)
{
    size_t index;

    if (value == NULL || length == NULL) {
        return 0;
    }
    for (index = 0U; index <= VPS_CREDENTIAL_VALUE_MAX_LENGTH; ++index) {
        if (value[index] == '\0') {
            if (!vps_utf8_is_valid(value, index, 1)) {
                return 0;
            }
            *length = index;
            return 1;
        }
    }
    return 0;
}

static void vps_registry_log_locked(VpsCredentialRegistry *registry,
                                    const char *phase,
                                    VpsCredentialFields fields,
                                    const char *status,
                                    VpsLogLevel level)
{
    static const char operation[] = "credential_provider";
    VpsLogEvent event;

    if (registry->logger == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 strlen(phase)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PROVIDER_ID,
                                 registry->provider_id) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_GENERATION,
                                 registry->generation) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PRESENCE_MASK,
                                 fields) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK) {
        return;
    }
    vps_logger_emit(registry->logger, &event);
}

static VpsCredentialRegistryResult vps_registry_lock(
    VpsCredentialRegistry *registry)
{
    return vps_platform_mutex_lock(registry->operations, &registry->mutex) ==
                   VPS_PLATFORM_OK
               ? VPS_CREDENTIAL_REGISTRY_OK
               : VPS_CREDENTIAL_REGISTRY_PLATFORM_ERROR;
}

static VpsCredentialRegistryResult vps_registry_unlock(
    VpsCredentialRegistry *registry)
{
    return vps_platform_mutex_unlock(registry->operations, &registry->mutex) ==
                   VPS_PLATFORM_OK
               ? VPS_CREDENTIAL_REGISTRY_OK
               : VPS_CREDENTIAL_REGISTRY_PLATFORM_ERROR;
}

static VpsCredentialRegistryResult vps_provider_validate(
    const VpsCredentialProvider *provider)
{
    uint32_t minimum_size =
        (uint32_t)(offsetof(VpsCredentialProvider, provider_context) +
                   sizeof(provider->provider_context));

    if (provider == NULL) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    if (vps_abi_validate_header(&provider->header, minimum_size,
                                VPS_API_VERSION) != VPS_ABI_VALID) {
        return VPS_CREDENTIAL_REGISTRY_ABI_INCOMPATIBLE;
    }
    if (provider->resolve == NULL || provider->release == NULL) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    if (provider->header.present_fields != 0U &&
        (provider->header.present_fields &
         (VPS_CREDENTIAL_PROVIDER_FIELD_RESOLVE |
          VPS_CREDENTIAL_PROVIDER_FIELD_RELEASE)) !=
            (VPS_CREDENTIAL_PROVIDER_FIELD_RESOLVE |
             VPS_CREDENTIAL_PROVIDER_FIELD_RELEASE)) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    return VPS_CREDENTIAL_REGISTRY_OK;
}

VpsCredentialRegistryResult vps_credential_registry_init(
    VpsCredentialRegistry *registry,
    const VpsPlatformOperations *operations,
    VpsLogger *logger)
{
    if (registry == NULL ||
        vps_platform_validate_operations(operations,
                                         VPS_PLATFORM_CAP_MUTEX) !=
            VPS_PLATFORM_OK) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    (void)memset(registry, 0, sizeof(*registry));
    registry->operations = operations;
    registry->logger = logger;
    if (vps_platform_mutex_init(operations, &registry->mutex) !=
        VPS_PLATFORM_OK) {
        return VPS_CREDENTIAL_REGISTRY_PLATFORM_ERROR;
    }
    registry->initialized = 1;
    return VPS_CREDENTIAL_REGISTRY_OK;
}

VpsCredentialRegistryResult vps_credential_registry_register(
    VpsCredentialRegistry *registry,
    uint64_t provider_id,
    const VpsCredentialProvider *provider)
{
    VpsCredentialRegistryResult result;

    if (registry == NULL || !registry->initialized || provider_id == 0U) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    result = vps_provider_validate(provider);
    if (result != VPS_CREDENTIAL_REGISTRY_OK) {
        return result;
    }
    result = vps_registry_lock(registry);
    if (result != VPS_CREDENTIAL_REGISTRY_OK) {
        return result;
    }
    if (registry->resolve_started) {
        vps_registry_log_locked(registry, "registration", 0U,
                                "replacement_forbidden", VPS_LOG_LEVEL_WARN);
        (void)vps_registry_unlock(registry);
        return VPS_CREDENTIAL_REGISTRY_REPLACEMENT_FORBIDDEN;
    }
    if (registry->generation == UINT64_MAX) {
        (void)vps_registry_unlock(registry);
        return VPS_CREDENTIAL_REGISTRY_BUSY;
    }
    (void)memset(&registry->provider, 0, sizeof(registry->provider));
    registry->provider.header = provider->header;
    registry->provider.resolve = provider->resolve;
    registry->provider.release = provider->release;
    if (provider->header.structure_size >=
        offsetof(VpsCredentialProvider, provider_context) +
            sizeof(provider->provider_context)) {
        registry->provider.provider_context = provider->provider_context;
    }
    registry->provider_id = provider_id;
    registry->generation += 1U;
    registry->registered = 1;
    vps_registry_log_locked(registry, "registration",
                            provider->header.present_fields, "registered",
                            VPS_LOG_LEVEL_INFO);
    return vps_registry_unlock(registry);
}

VpsCredentialRegistryResult vps_resolved_credential_init(
    VpsResolvedCredential *resolved,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger)
{
    if (resolved == NULL) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    (void)memset(resolved, 0, sizeof(*resolved));
    if (vps_sensitive_memory_init(&resolved->storage, allocator, operations,
                                  logger) != VPS_SECURE_MEMORY_OK) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    resolved->config.header.structure_size =
        (uint32_t)sizeof(resolved->config);
    resolved->config.header.api_version = VPS_API_VERSION;
    resolved->initialized = 1;
    return VPS_CREDENTIAL_REGISTRY_OK;
}

static VpsCredentialRegistryResult vps_copy_config(
    const VpsCredentialConfig *source,
    VpsResolvedCredential *resolved)
{
    size_t lengths[sizeof(vps_credential_fields) /
                   sizeof(vps_credential_fields[0])] = {0};
    size_t total_size = 0U;
    size_t index;
    char *cursor;

    if (source == NULL ||
        vps_abi_validate_header(&source->header,
                                (uint32_t)sizeof(VpsAbiHeader),
                                VPS_API_VERSION) != VPS_ABI_VALID ||
        (source->header.present_fields & VPS_CREDENTIAL_FIELDS_CURRENT) == 0U) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_CONFIG;
    }
    for (index = 0U;
         index < sizeof(vps_credential_fields) / sizeof(vps_credential_fields[0]);
         ++index) {
        const VpsCredentialFieldDescriptor *field = &vps_credential_fields[index];
        const char *value = vps_config_get_string(source, field->offset);
        int present = (source->header.present_fields & field->bit) != 0U;

        if ((present && value == NULL) ||
            (present && !vps_bounded_string_length(value, &lengths[index])) ||
            (present && vps_size_add(total_size, lengths[index] + 1U,
                                     &total_size) != VPS_MEMORY_OK)) {
            return VPS_CREDENTIAL_REGISTRY_INVALID_CONFIG;
        }
    }
    if (vps_sensitive_memory_allocate(&resolved->storage, total_size) !=
        VPS_SECURE_MEMORY_OK) {
        return VPS_CREDENTIAL_REGISTRY_OUT_OF_MEMORY;
    }
    cursor = (char *)vps_sensitive_memory_data(&resolved->storage);
    for (index = 0U;
         index < sizeof(vps_credential_fields) / sizeof(vps_credential_fields[0]);
         ++index) {
        const VpsCredentialFieldDescriptor *field = &vps_credential_fields[index];
        const char *value = vps_config_get_string(source, field->offset);

        if ((source->header.present_fields & field->bit) != 0U) {
            (void)memcpy(cursor, value, lengths[index] + 1U);
            vps_config_set_string(&resolved->config, field->offset, cursor);
            cursor += lengths[index] + 1U;
        }
    }
    resolved->config.header.present_fields =
        source->header.present_fields & VPS_CREDENTIAL_FIELDS_CURRENT;
    return VPS_CREDENTIAL_REGISTRY_OK;
}

static void vps_resolve_finish(VpsCredentialRegistry *registry,
                               VpsCredentialFields fields,
                               const char *status,
                               VpsLogLevel level)
{
    if (vps_registry_lock(registry) != VPS_CREDENTIAL_REGISTRY_OK) {
        return;
    }
    if (registry->active_resolves != 0U) {
        registry->active_resolves -= 1U;
    }
    vps_registry_log_locked(registry, "resolve", fields, status, level);
    (void)vps_registry_unlock(registry);
}

VpsCredentialRegistryResult vps_credential_registry_resolve(
    VpsCredentialRegistry *registry,
    const char *credential_ref,
    uint32_t credential_ref_length,
    VpsResolvedCredential *resolved)
{
    VpsCredentialProvider provider;
    VpsCredentialLease lease;
    VpsCredentialRegistryResult result;
    int32_t provider_result;
    uint64_t provider_id;
    uint64_t generation;
    VpsCredentialFields fields = 0U;

    if (registry == NULL || !registry->initialized || resolved == NULL ||
        !resolved->initialized || resolved->storage.storage.owned ||
        credential_ref == NULL || credential_ref_length == 0U ||
        credential_ref_length > VPS_CREDENTIAL_REFERENCE_MAX_LENGTH ||
        !vps_utf8_is_valid(credential_ref, credential_ref_length, 1)) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    result = vps_registry_lock(registry);
    if (result != VPS_CREDENTIAL_REGISTRY_OK) {
        return result;
    }
    if (!registry->registered) {
        (void)vps_registry_unlock(registry);
        return VPS_CREDENTIAL_REGISTRY_NOT_REGISTERED;
    }
    registry->resolve_started = 1;
    if (registry->active_resolves == UINT64_MAX) {
        (void)vps_registry_unlock(registry);
        return VPS_CREDENTIAL_REGISTRY_BUSY;
    }
    registry->active_resolves += 1U;
    provider = registry->provider;
    provider_id = registry->provider_id;
    generation = registry->generation;
    result = vps_registry_unlock(registry);
    if (result != VPS_CREDENTIAL_REGISTRY_OK) {
        return result;
    }

    (void)memset(&lease, 0, sizeof(lease));
    lease.header.structure_size = (uint32_t)sizeof(lease);
    lease.header.api_version = VPS_API_VERSION;
    lease.header.present_fields = VPS_CREDENTIAL_LEASE_FIELDS_CURRENT;
    provider_result = provider.resolve(provider.provider_context,
                                       credential_ref,
                                       credential_ref_length, &lease);
    if (provider_result != VPS_CREDENTIAL_PROVIDER_OK) {
        vps_resolve_finish(registry, 0U, "provider_failed",
                           VPS_LOG_LEVEL_WARN);
        return VPS_CREDENTIAL_REGISTRY_RESOLVE_FAILED;
    }

    if (vps_abi_validate_header(
            &lease.header,
            (uint32_t)(offsetof(VpsCredentialLease, config) +
                       sizeof(lease.config)),
            VPS_API_VERSION) != VPS_ABI_VALID ||
        (lease.header.present_fields != 0U &&
         (lease.header.present_fields & VPS_CREDENTIAL_LEASE_FIELD_CONFIG) ==
             0U)) {
        result = VPS_CREDENTIAL_REGISTRY_ABI_INCOMPATIBLE;
    } else {
        if (lease.config != NULL) {
            fields = lease.config->header.present_fields;
        }
        result = vps_copy_config(lease.config, resolved);
    }
    provider.release(provider.provider_context, &lease);

    if (result == VPS_CREDENTIAL_REGISTRY_OK) {
        resolved->provider_id = provider_id;
        resolved->generation = generation;
        vps_resolve_finish(registry, fields, "resolved", VPS_LOG_LEVEL_DEBUG);
    } else {
        vps_resolve_finish(registry, fields,
                           vps_credential_registry_result_name(result),
                           VPS_LOG_LEVEL_ERROR);
    }
    return result;
}

VpsCredentialRegistryResult vps_resolved_credential_cleanup(
    VpsResolvedCredential *resolved)
{
    if (resolved == NULL || !resolved->initialized) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    if (vps_sensitive_memory_release(&resolved->storage) !=
        VPS_SECURE_MEMORY_OK) {
        return VPS_CREDENTIAL_REGISTRY_CLEANUP_FAILED;
    }
    (void)memset(&resolved->config, 0, sizeof(resolved->config));
    resolved->config.header.structure_size =
        (uint32_t)sizeof(resolved->config);
    resolved->config.header.api_version = VPS_API_VERSION;
    resolved->provider_id = 0U;
    resolved->generation = 0U;
    return VPS_CREDENTIAL_REGISTRY_OK;
}

VpsCredentialRegistryResult vps_credential_registry_cleanup(
    VpsCredentialRegistry *registry)
{
    VpsCredentialRegistryResult result;

    if (registry == NULL) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    if (!registry->initialized) {
        return VPS_CREDENTIAL_REGISTRY_OK;
    }
    result = vps_registry_lock(registry);
    if (result != VPS_CREDENTIAL_REGISTRY_OK) {
        return result;
    }
    if (registry->active_resolves != 0U) {
        (void)vps_registry_unlock(registry);
        return VPS_CREDENTIAL_REGISTRY_BUSY;
    }
    registry->registered = 0;
    (void)memset(&registry->provider, 0, sizeof(registry->provider));
    result = vps_registry_unlock(registry);
    if (result != VPS_CREDENTIAL_REGISTRY_OK) {
        return result;
    }
    if (vps_platform_mutex_destroy(registry->operations, &registry->mutex) !=
        VPS_PLATFORM_OK) {
        return VPS_CREDENTIAL_REGISTRY_PLATFORM_ERROR;
    }
    registry->initialized = 0;
    return VPS_CREDENTIAL_REGISTRY_OK;
}

const char *vps_credential_registry_result_name(
    VpsCredentialRegistryResult result)
{
    static const char *const names[] = {
        "ok", "invalid_argument", "abi_incompatible", "not_registered",
        "replacement_forbidden", "resolve_failed", "invalid_config",
        "out_of_memory", "busy", "platform_error", "cleanup_failed"};

    if (result < VPS_CREDENTIAL_REGISTRY_OK ||
        result > VPS_CREDENTIAL_REGISTRY_CLEANUP_FAILED) {
        return "unknown";
    }
    return names[(size_t)result];
}
