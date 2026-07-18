#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

#include "vps_wincred_provider.h"

#include <stddef.h>
#include <string.h>

typedef struct VpsWinCredLeaseOwner {
    VpsSensitiveMemory values;
    VpsCredentialConfig config;
} VpsWinCredLeaseOwner;

typedef struct VpsWinCredFieldDescriptor {
    VpsCredentialFields bit;
    size_t config_offset;
} VpsWinCredFieldDescriptor;

typedef enum VpsWinCredTargetResult {
    VPS_WINCRED_TARGET_OK = 0,
    VPS_WINCRED_TARGET_INVALID = 1,
    VPS_WINCRED_TARGET_ERROR = 2
} VpsWinCredTargetResult;

#define VPS_WINCRED_CONFIG_OFFSET(member) offsetof(VpsCredentialConfig, member)

static const VpsWinCredFieldDescriptor vps_wincred_fields[] = {
    {VPS_CREDENTIAL_FIELD_HOSTS, VPS_WINCRED_CONFIG_OFFSET(hosts)},
    {VPS_CREDENTIAL_FIELD_PORTS, VPS_WINCRED_CONFIG_OFFSET(ports)},
    {VPS_CREDENTIAL_FIELD_USER, VPS_WINCRED_CONFIG_OFFSET(user)},
    {VPS_CREDENTIAL_FIELD_PASSWORD, VPS_WINCRED_CONFIG_OFFSET(password)},
    {VPS_CREDENTIAL_FIELD_DBNAME, VPS_WINCRED_CONFIG_OFFSET(dbname)},
    {VPS_CREDENTIAL_FIELD_SERVICE, VPS_WINCRED_CONFIG_OFFSET(service)},
    {VPS_CREDENTIAL_FIELD_SERVICE_FILE,
     VPS_WINCRED_CONFIG_OFFSET(service_file)},
    {VPS_CREDENTIAL_FIELD_SSLMODE, VPS_WINCRED_CONFIG_OFFSET(sslmode)},
    {VPS_CREDENTIAL_FIELD_SSLROOTCERT,
     VPS_WINCRED_CONFIG_OFFSET(sslrootcert)},
    {VPS_CREDENTIAL_FIELD_SSLCERT, VPS_WINCRED_CONFIG_OFFSET(sslcert)},
    {VPS_CREDENTIAL_FIELD_SSLKEY, VPS_WINCRED_CONFIG_OFFSET(sslkey)},
    {VPS_CREDENTIAL_FIELD_SSLCRL, VPS_WINCRED_CONFIG_OFFSET(sslcrl)},
    {VPS_CREDENTIAL_FIELD_CHANNEL_BINDING,
     VPS_WINCRED_CONFIG_OFFSET(channel_binding)},
    {VPS_CREDENTIAL_FIELD_TARGET_SESSION_ATTRS,
     VPS_WINCRED_CONFIG_OFFSET(target_session_attrs)},
    {VPS_CREDENTIAL_FIELD_CONNECT_TIMEOUT,
     VPS_WINCRED_CONFIG_OFFSET(connect_timeout)},
    {VPS_CREDENTIAL_FIELD_STATEMENT_TIMEOUT,
     VPS_WINCRED_CONFIG_OFFSET(statement_timeout)},
    {VPS_CREDENTIAL_FIELD_LOCK_TIMEOUT,
     VPS_WINCRED_CONFIG_OFFSET(lock_timeout)},
    {VPS_CREDENTIAL_FIELD_APPLICATION_NAME,
     VPS_WINCRED_CONFIG_OFFSET(application_name)},
    {VPS_CREDENTIAL_FIELD_SEARCH_PATH,
     VPS_WINCRED_CONFIG_OFFSET(search_path)}};

_Static_assert(CRED_MAX_CREDENTIAL_BLOB_SIZE == VPS_WINCRED_BLOB_MAX_LENGTH,
               "WinCred blob limit changed");
_Static_assert(sizeof(wchar_t) == sizeof(uint16_t),
               "Windows UTF-16 code unit must be 16 bits");

static uint16_t vps_wincred_read_u16(const unsigned char *value)
{
    return (uint16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8));
}

static uint32_t vps_wincred_read_u32(const unsigned char *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static uint64_t vps_wincred_read_u64(const unsigned char *value)
{
    return (uint64_t)vps_wincred_read_u32(value) |
           ((uint64_t)vps_wincred_read_u32(value + 4U) << 32);
}

static int vps_wincred_utf8_valid(const char *value,
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

static const VpsWinCredFieldDescriptor *vps_wincred_find_field(
    VpsCredentialFields bit)
{
    size_t index;

    for (index = 0U;
         index < sizeof(vps_wincred_fields) / sizeof(vps_wincred_fields[0]);
         ++index) {
        if (vps_wincred_fields[index].bit == bit) {
            return &vps_wincred_fields[index];
        }
    }
    return NULL;
}

static void vps_wincred_config_set(VpsCredentialConfig *config,
                                   size_t offset,
                                   const char *value)
{
    const char **member = (const char **)((unsigned char *)config + offset);
    *member = value;
}

static VpsWinCredReadResult vps_wincred_native_read(
    void *api_context,
    const uint16_t *target,
    size_t target_length,
    VpsWinCredRecord *record)
{
    PCREDENTIALW credential = NULL;

    (void)api_context;
    (void)target_length;
    if (!CredReadW((LPCWSTR)target, CRED_TYPE_GENERIC, 0U, &credential)) {
        return GetLastError() == ERROR_NOT_FOUND ? VPS_WINCRED_READ_NOT_FOUND
                                                 : VPS_WINCRED_READ_ERROR;
    }
    record->blob = credential->CredentialBlob;
    record->blob_size = credential->CredentialBlobSize;
    record->native_record = credential;
    return VPS_WINCRED_READ_OK;
}

static void vps_wincred_native_release(void *api_context,
                                       VpsWinCredRecord *record)
{
    (void)api_context;
    CredFree(record->native_record);
    (void)memset(record, 0, sizeof(*record));
}

static const VpsWinCredApi vps_wincred_native_api = {
    vps_wincred_native_read, vps_wincred_native_release};

static void vps_wincred_log_locked(VpsWinCredProviderContext *context,
                                   const char *phase,
                                   VpsCredentialFields fields,
                                   const char *status,
                                   VpsLogLevel level)
{
    static const char operation[] = "wincred_provider";
    VpsLogEvent event;

    if (context->logger == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 strlen(phase)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PRESENCE_MASK,
                                 fields) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK) {
        return;
    }
    vps_logger_emit(context->logger, &event);
}

static int vps_wincred_begin_resolve(VpsWinCredProviderContext *context)
{
    if (vps_platform_mutex_lock(context->operations, &context->mutex) !=
        VPS_PLATFORM_OK) {
        return 0;
    }
    if (!context->initialized || context->active_resolves == UINT64_MAX ||
        context->active_leases == UINT64_MAX) {
        (void)vps_platform_mutex_unlock(context->operations, &context->mutex);
        return 0;
    }
    context->active_resolves += 1U;
    (void)vps_platform_mutex_unlock(context->operations, &context->mutex);
    return 1;
}

static void vps_wincred_finish_resolve(VpsWinCredProviderContext *context,
                                       VpsCredentialFields fields,
                                       const char *status,
                                       VpsLogLevel level,
                                       int lease_created)
{
    if (vps_platform_mutex_lock(context->operations, &context->mutex) !=
        VPS_PLATFORM_OK) {
        return;
    }
    if (context->active_resolves != 0U) {
        context->active_resolves -= 1U;
    }
    if (lease_created && context->active_leases != UINT64_MAX) {
        context->active_leases += 1U;
    }
    vps_wincred_log_locked(context, "resolve", fields, status, level);
    (void)vps_platform_mutex_unlock(context->operations, &context->mutex);
}

static VpsWinCredTargetResult vps_wincred_target_to_utf16(
    VpsWinCredProviderContext *context,
    const char *credential_ref,
    uint32_t credential_ref_length,
    VpsSensitiveMemory *target_memory,
    uint16_t **target,
    size_t *target_length)
{
    int wide_length;
    size_t allocation_size;
    wchar_t *wide_target;

    if (credential_ref == NULL || credential_ref_length == 0U ||
        credential_ref_length > VPS_CREDENTIAL_REFERENCE_MAX_LENGTH ||
        !vps_wincred_utf8_valid(credential_ref, credential_ref_length, 1)) {
        return VPS_WINCRED_TARGET_INVALID;
    }
    if (vps_sensitive_memory_init(target_memory, &context->allocator,
                                  context->operations, NULL) !=
        VPS_SECURE_MEMORY_OK) {
        return VPS_WINCRED_TARGET_ERROR;
    }
    wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      credential_ref,
                                      (int)credential_ref_length, NULL, 0);
    if (wide_length <= 0 ||
        vps_size_add((size_t)wide_length, 1U, &allocation_size) !=
            VPS_MEMORY_OK ||
        vps_size_multiply(allocation_size, sizeof(wchar_t),
                          &allocation_size) != VPS_MEMORY_OK ||
        vps_sensitive_memory_allocate(target_memory, allocation_size) !=
            VPS_SECURE_MEMORY_OK) {
        return wide_length <= 0 ? VPS_WINCRED_TARGET_INVALID
                                : VPS_WINCRED_TARGET_ERROR;
    }
    wide_target = (wchar_t *)vps_sensitive_memory_data(target_memory);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, credential_ref,
                            (int)credential_ref_length, wide_target,
                            wide_length) != wide_length) {
        return VPS_WINCRED_TARGET_ERROR;
    }
    wide_target[wide_length] = L'\0';
    *target = (uint16_t *)wide_target;
    *target_length = (size_t)wide_length;
    return VPS_WINCRED_TARGET_OK;
}

static int vps_wincred_validate_blob(const unsigned char *blob,
                                     size_t blob_size,
                                     VpsCredentialFields *fields,
                                     size_t *values_size)
{
    uint16_t field_count;
    VpsCredentialFields seen = 0U;
    size_t cursor = VPS_WINCRED_FORMAT_HEADER_SIZE;
    size_t index;
    size_t total = 0U;

    if (blob == NULL || blob_size < VPS_WINCRED_FORMAT_HEADER_SIZE ||
        blob_size > VPS_WINCRED_BLOB_MAX_LENGTH ||
        vps_wincred_read_u32(blob) != VPS_WINCRED_FORMAT_MAGIC ||
        vps_wincred_read_u16(blob + 4U) != VPS_WINCRED_FORMAT_VERSION) {
        return 0;
    }
    field_count = vps_wincred_read_u16(blob + 6U);
    if (field_count == 0U ||
        field_count > sizeof(vps_wincred_fields) / sizeof(vps_wincred_fields[0])) {
        return 0;
    }
    for (index = 0U; index < field_count; ++index) {
        VpsCredentialFields bit;
        uint32_t length;
        size_t entry_size;

        if (cursor > blob_size ||
            VPS_WINCRED_FORMAT_ENTRY_SIZE > blob_size - cursor) {
            return 0;
        }
        bit = vps_wincred_read_u64(blob + cursor);
        length = vps_wincred_read_u32(blob + cursor + 8U);
        if (vps_wincred_read_u32(blob + cursor + 12U) != 0U ||
            vps_wincred_find_field(bit) == NULL || (seen & bit) != 0U ||
            length > VPS_CREDENTIAL_VALUE_MAX_LENGTH ||
            vps_size_add(VPS_WINCRED_FORMAT_ENTRY_SIZE, (size_t)length,
                         &entry_size) != VPS_MEMORY_OK ||
            entry_size > blob_size - cursor ||
            !vps_wincred_utf8_valid((const char *)(blob + cursor +
                                                   VPS_WINCRED_FORMAT_ENTRY_SIZE),
                                    length, 1) ||
            vps_size_add(total, (size_t)length + 1U, &total) !=
                VPS_MEMORY_OK) {
            return 0;
        }
        seen |= bit;
        cursor += entry_size;
    }
    if (cursor != blob_size) {
        return 0;
    }
    *fields = seen;
    *values_size = total;
    return 1;
}

static VpsWinCredLeaseOwner *vps_wincred_decode_blob(
    VpsWinCredProviderContext *context,
    const unsigned char *blob,
    size_t blob_size,
    VpsCredentialFields *fields)
{
    VpsWinCredLeaseOwner *owner = NULL;
    void *owner_memory = NULL;
    size_t values_size;
    size_t cursor = VPS_WINCRED_FORMAT_HEADER_SIZE;
    uint16_t field_count;
    size_t index;
    char *destination;

    if (!vps_wincred_validate_blob(blob, blob_size, fields, &values_size) ||
        vps_memory_allocate(&context->allocator, sizeof(*owner),
                            &owner_memory) != VPS_MEMORY_OK) {
        return NULL;
    }
    owner = (VpsWinCredLeaseOwner *)owner_memory;
    (void)memset(owner, 0, sizeof(*owner));
    if (vps_sensitive_memory_init(&owner->values, &context->allocator,
                                  context->operations, NULL) !=
            VPS_SECURE_MEMORY_OK ||
        vps_sensitive_memory_allocate(&owner->values, values_size) !=
            VPS_SECURE_MEMORY_OK) {
        vps_memory_release(&context->allocator, &owner_memory, sizeof(*owner));
        return NULL;
    }
    owner->config.header.structure_size = (uint32_t)sizeof(owner->config);
    owner->config.header.api_version = VPS_API_VERSION;
    owner->config.header.present_fields = *fields;
    destination = (char *)vps_sensitive_memory_data(&owner->values);
    field_count = vps_wincred_read_u16(blob + 6U);
    for (index = 0U; index < field_count; ++index) {
        VpsCredentialFields bit = vps_wincred_read_u64(blob + cursor);
        uint32_t length = vps_wincred_read_u32(blob + cursor + 8U);
        const VpsWinCredFieldDescriptor *field = vps_wincred_find_field(bit);

        (void)memcpy(destination,
                     blob + cursor + VPS_WINCRED_FORMAT_ENTRY_SIZE, length);
        destination[length] = '\0';
        vps_wincred_config_set(&owner->config, field->config_offset,
                               destination);
        destination += (size_t)length + 1U;
        cursor += VPS_WINCRED_FORMAT_ENTRY_SIZE + (size_t)length;
    }
    return owner;
}

static int vps_wincred_owner_release(VpsWinCredProviderContext *context,
                                     VpsWinCredLeaseOwner **owner)
{
    void *owner_memory;

    if (owner == NULL || *owner == NULL) {
        return 1;
    }
    if (vps_sensitive_memory_release(&(*owner)->values) !=
        VPS_SECURE_MEMORY_OK) {
        return 0;
    }
    owner_memory = *owner;
    (void)memset(owner_memory, 0, sizeof(**owner));
    vps_memory_release(&context->allocator, &owner_memory, sizeof(**owner));
    *owner = NULL;
    return 1;
}

static int32_t VPS_CALL vps_wincred_resolve(void *provider_context,
                                            const char *credential_ref,
                                            uint32_t credential_ref_length,
                                            VpsCredentialLease *lease)
{
    VpsWinCredProviderContext *context =
        (VpsWinCredProviderContext *)provider_context;
    VpsSensitiveMemory target_memory = {0};
    uint16_t *target = NULL;
    size_t target_length = 0U;
    VpsWinCredRecord record = {0};
    VpsWinCredReadResult read_result;
    VpsWinCredLeaseOwner *owner = NULL;
    VpsCredentialFields fields = 0U;
    int32_t result = VPS_CREDENTIAL_PROVIDER_ERROR;
    const char *status;
    int record_acquired = 0;
    VpsWinCredTargetResult target_result;

    if (context == NULL || lease == NULL || !context->initialized ||
        !vps_wincred_begin_resolve(context)) {
        return VPS_CREDENTIAL_PROVIDER_UNAVAILABLE;
    }
    target_result = vps_wincred_target_to_utf16(
        context, credential_ref, credential_ref_length, &target_memory,
        &target, &target_length);
    if (target_result != VPS_WINCRED_TARGET_OK) {
        result = target_result == VPS_WINCRED_TARGET_INVALID
                     ? VPS_CREDENTIAL_PROVIDER_INVALID_REFERENCE
                     : VPS_CREDENTIAL_PROVIDER_ERROR;
        status = target_result == VPS_WINCRED_TARGET_INVALID
                     ? "invalid_reference"
                     : "conversion_failed";
        goto cleanup;
    }
    read_result = context->api.read(context->api_context, target, target_length,
                                    &record);
    if (read_result == VPS_WINCRED_READ_NOT_FOUND) {
        result = VPS_CREDENTIAL_PROVIDER_NOT_FOUND;
        status = "not_found";
        goto cleanup;
    }
    if (read_result != VPS_WINCRED_READ_OK) {
        status = "read_failed";
        goto cleanup;
    }
    record_acquired = 1;
    if (record.native_record == NULL ||
        (record.blob == NULL && record.blob_size != 0U)) {
        status = "read_failed";
        goto cleanup;
    }
    owner = vps_wincred_decode_blob(context, record.blob, record.blob_size,
                                    &fields);
    if (owner == NULL) {
        status = "malformed_or_oom";
        goto cleanup;
    }
    result = VPS_CREDENTIAL_PROVIDER_OK;
    status = "resolved";

cleanup:
    if (record_acquired) {
        if (vps_platform_secure_zero(context->operations, record.blob,
                                     record.blob_size) != VPS_PLATFORM_OK) {
            result = VPS_CREDENTIAL_PROVIDER_ERROR;
            status = "zero_failed";
        }
        context->api.release_record(context->api_context, &record);
    }
    if (target_memory.initialized &&
        vps_sensitive_memory_release(&target_memory) !=
            VPS_SECURE_MEMORY_OK) {
        result = VPS_CREDENTIAL_PROVIDER_ERROR;
        status = "zero_failed";
    }
    if (result == VPS_CREDENTIAL_PROVIDER_OK) {
        lease->header.structure_size = (uint32_t)sizeof(*lease);
        lease->header.api_version = VPS_API_VERSION;
        lease->header.present_fields = VPS_CREDENTIAL_LEASE_FIELDS_CURRENT;
        lease->config = &owner->config;
        lease->provider_lease = owner;
        owner = NULL;
    }
    (void)vps_wincred_owner_release(context, &owner);
    vps_wincred_finish_resolve(context, fields, status,
                               result == VPS_CREDENTIAL_PROVIDER_OK
                                   ? VPS_LOG_LEVEL_DEBUG
                                   : VPS_LOG_LEVEL_WARN,
                               result == VPS_CREDENTIAL_PROVIDER_OK);
    return result;
}

static void VPS_CALL vps_wincred_release(void *provider_context,
                                         VpsCredentialLease *lease)
{
    VpsWinCredProviderContext *context =
        (VpsWinCredProviderContext *)provider_context;
    VpsWinCredLeaseOwner *owner;
    int had_owner;

    if (context == NULL || lease == NULL || !context->initialized) {
        return;
    }
    owner = (VpsWinCredLeaseOwner *)lease->provider_lease;
    had_owner = owner != NULL;
    if (!vps_wincred_owner_release(context, &owner)) {
        return;
    }
    if (had_owner &&
        vps_platform_mutex_lock(context->operations, &context->mutex) ==
            VPS_PLATFORM_OK) {
        if (context->active_leases != 0U) {
            context->active_leases -= 1U;
        }
        (void)vps_platform_mutex_unlock(context->operations, &context->mutex);
    }
    (void)memset(lease, 0, sizeof(*lease));
}

VpsCredentialRegistryResult vps_wincred_provider_init_with_api(
    VpsWinCredProviderContext *context,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger,
    const VpsWinCredApi *api,
    void *api_context)
{
    if (context == NULL || !vps_allocator_is_valid(allocator) || api == NULL ||
        api->read == NULL || api->release_record == NULL ||
        vps_platform_validate_operations(
            operations, VPS_PLATFORM_CAP_MUTEX | VPS_PLATFORM_CAP_SECURE_ZERO) !=
            VPS_PLATFORM_OK) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    (void)memset(context, 0, sizeof(*context));
    context->allocator = *allocator;
    context->operations = operations;
    context->logger = logger;
    context->api = *api;
    context->api_context = api_context;
    if (vps_platform_mutex_init(operations, &context->mutex) !=
        VPS_PLATFORM_OK) {
        return VPS_CREDENTIAL_REGISTRY_PLATFORM_ERROR;
    }
    context->initialized = 1;
    return VPS_CREDENTIAL_REGISTRY_OK;
}

VpsCredentialRegistryResult vps_wincred_provider_init(
    VpsWinCredProviderContext *context,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger)
{
    return vps_wincred_provider_init_with_api(
        context, allocator, operations, logger, &vps_wincred_native_api, NULL);
}

VpsCredentialRegistryResult vps_wincred_provider_make(
    VpsWinCredProviderContext *context,
    VpsCredentialProvider *provider)
{
    if (context == NULL || !context->initialized || provider == NULL) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    (void)memset(provider, 0, sizeof(*provider));
    provider->header.structure_size = (uint32_t)sizeof(*provider);
    provider->header.api_version = VPS_API_VERSION;
    provider->header.present_fields = VPS_CREDENTIAL_PROVIDER_FIELDS_CURRENT;
    provider->resolve = vps_wincred_resolve;
    provider->release = vps_wincred_release;
    provider->provider_context = context;
    return VPS_CREDENTIAL_REGISTRY_OK;
}

VpsCredentialRegistryResult vps_wincred_provider_cleanup(
    VpsWinCredProviderContext *context)
{
    if (context == NULL) {
        return VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return VPS_CREDENTIAL_REGISTRY_OK;
    }
    if (vps_platform_mutex_lock(context->operations, &context->mutex) !=
        VPS_PLATFORM_OK) {
        return VPS_CREDENTIAL_REGISTRY_PLATFORM_ERROR;
    }
    if (context->active_resolves != 0U || context->active_leases != 0U) {
        (void)vps_platform_mutex_unlock(context->operations, &context->mutex);
        return VPS_CREDENTIAL_REGISTRY_BUSY;
    }
    context->initialized = 0;
    (void)vps_platform_mutex_unlock(context->operations, &context->mutex);
    if (vps_platform_mutex_destroy(context->operations, &context->mutex) !=
        VPS_PLATFORM_OK) {
        return VPS_CREDENTIAL_REGISTRY_PLATFORM_ERROR;
    }
    return VPS_CREDENTIAL_REGISTRY_OK;
}
