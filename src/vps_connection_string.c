#include "vps_connection_string.h"

#include "vps_private_api.h"

#include <stddef.h>
#include <string.h>

typedef struct VpsConnectionField {
    VpsCredentialFields bit;
    size_t offset;
    const char *profile_suffix;
} VpsConnectionField;

#define VPS_CONFIG_OFFSET(member) offsetof(VpsCredentialConfig, member)

static const VpsConnectionField vps_connection_fields[] = {
    {VPS_CREDENTIAL_FIELD_HOSTS, VPS_CONFIG_OFFSET(hosts), "HOST"},
    {VPS_CREDENTIAL_FIELD_PORTS, VPS_CONFIG_OFFSET(ports), "PORT"},
    {VPS_CREDENTIAL_FIELD_USER, VPS_CONFIG_OFFSET(user), "USER"},
    {VPS_CREDENTIAL_FIELD_PASSWORD, VPS_CONFIG_OFFSET(password), "PASSWORD"},
    {VPS_CREDENTIAL_FIELD_DBNAME, VPS_CONFIG_OFFSET(dbname), "DBNAME"},
    {VPS_CREDENTIAL_FIELD_SERVICE, VPS_CONFIG_OFFSET(service), "SERVICE"},
    {VPS_CREDENTIAL_FIELD_SERVICE_FILE, VPS_CONFIG_OFFSET(service_file),
     "SERVICE_FILE"},
    {VPS_CREDENTIAL_FIELD_SSLMODE, VPS_CONFIG_OFFSET(sslmode), "SSLMODE"},
    {VPS_CREDENTIAL_FIELD_SSLROOTCERT, VPS_CONFIG_OFFSET(sslrootcert),
     "SSLROOTCERT"},
    {VPS_CREDENTIAL_FIELD_SSLCERT, VPS_CONFIG_OFFSET(sslcert), "SSLCERT"},
    {VPS_CREDENTIAL_FIELD_SSLKEY, VPS_CONFIG_OFFSET(sslkey), "SSLKEY"},
    {VPS_CREDENTIAL_FIELD_SSLCRL, VPS_CONFIG_OFFSET(sslcrl), "SSLCRL"},
    {VPS_CREDENTIAL_FIELD_CHANNEL_BINDING,
     VPS_CONFIG_OFFSET(channel_binding), "CHANNEL_BINDING"},
    {VPS_CREDENTIAL_FIELD_TARGET_SESSION_ATTRS,
     VPS_CONFIG_OFFSET(target_session_attrs), "TARGET_SESSION_ATTRS"},
    {VPS_CREDENTIAL_FIELD_CONNECT_TIMEOUT,
     VPS_CONFIG_OFFSET(connect_timeout), "CONNECT_TIMEOUT"},
    {VPS_CREDENTIAL_FIELD_STATEMENT_TIMEOUT,
     VPS_CONFIG_OFFSET(statement_timeout), "STATEMENT_TIMEOUT"},
    {VPS_CREDENTIAL_FIELD_LOCK_TIMEOUT, VPS_CONFIG_OFFSET(lock_timeout),
     "LOCK_TIMEOUT"},
    {VPS_CREDENTIAL_FIELD_APPLICATION_NAME,
     VPS_CONFIG_OFFSET(application_name), "APPLICATION_NAME"},
    {VPS_CREDENTIAL_FIELD_SEARCH_PATH, VPS_CONFIG_OFFSET(search_path),
     "SEARCH_PATH"}};

static const char *vps_config_get(const VpsCredentialConfig *config,
                                  size_t offset)
{
    return *(const char *const *)((const unsigned char *)config + offset);
}

static void vps_config_set(VpsCredentialConfig *config,
                           size_t offset,
                           const char *value)
{
    *(const char **)((unsigned char *)config + offset) = value;
}

static int vps_value_length(const char *value, size_t *length)
{
    size_t index;
    if (value == NULL || length == NULL) {
        return 0;
    }
    for (index = 0U; index <= VPS_CREDENTIAL_VALUE_MAX_LENGTH; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte == 0U) {
            *length = index;
            return index != 0U;
        }
        if (byte < 0x20U || byte == 0x7fU) {
            return 0;
        }
    }
    return 0;
}

static VpsConnectionStringResult vps_copy_config(
    VpsConnectionConfig *destination,
    const VpsCredentialConfig *source)
{
    size_t lengths[sizeof(vps_connection_fields) /
                   sizeof(vps_connection_fields[0])] = {0};
    size_t total = 0U;
    size_t index;
    char *cursor;

    if (source == NULL || source->header.api_version != VPS_API_VERSION ||
        (source->header.present_fields & ~VPS_CREDENTIAL_FIELDS_CURRENT) != 0U ||
        (source->header.present_fields & VPS_CREDENTIAL_FIELDS_CURRENT) == 0U) {
        return VPS_CONNECTION_STRING_INVALID_VALUE;
    }
    for (index = 0U; index < sizeof(vps_connection_fields) /
                                  sizeof(vps_connection_fields[0]); ++index) {
        const VpsConnectionField *field = &vps_connection_fields[index];
        if ((source->header.present_fields & field->bit) != 0U) {
            const char *value = vps_config_get(source, field->offset);
            if (!vps_value_length(value, &lengths[index]) ||
                vps_size_add(total, lengths[index] + 1U, &total) !=
                    VPS_MEMORY_OK) {
                return VPS_CONNECTION_STRING_INVALID_VALUE;
            }
        }
    }
    if (vps_sensitive_memory_allocate(&destination->storage, total) !=
        VPS_SECURE_MEMORY_OK) {
        return VPS_CONNECTION_STRING_OUT_OF_MEMORY;
    }
    cursor = (char *)vps_sensitive_memory_data(&destination->storage);
    for (index = 0U; index < sizeof(vps_connection_fields) /
                                  sizeof(vps_connection_fields[0]); ++index) {
        const VpsConnectionField *field = &vps_connection_fields[index];
        if ((source->header.present_fields & field->bit) != 0U) {
            const char *value = vps_config_get(source, field->offset);
            (void)memcpy(cursor, value, lengths[index] + 1U);
            vps_config_set(&destination->config, field->offset, cursor);
            cursor += lengths[index] + 1U;
        }
    }
    destination->config.header.present_fields =
        source->header.present_fields & VPS_CREDENTIAL_FIELDS_CURRENT;
    return VPS_CONNECTION_STRING_OK;
}

static VpsConnectionStringResult vps_consume_conninfo(void *context,
                                                       const VpsCredentialConfig *config)
{
    return vps_copy_config((VpsConnectionConfig *)context, config);
}

static int vps_normalize_profile(const char *value,
                                 size_t length,
                                 char output[VPS_PROFILE_NAME_MAX_LENGTH + 1U])
{
    size_t index;
    if (value == NULL || length == 0U || length > VPS_PROFILE_NAME_MAX_LENGTH) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte >= 'a' && byte <= 'z') {
            output[index] = (char)(byte - 'a' + 'A');
        } else if ((byte >= 'A' && byte <= 'Z') ||
                   (byte >= '0' && byte <= '9') || byte == '_') {
            output[index] = (char)byte;
        } else if (byte == '-' || byte == '.') {
            output[index] = '_';
        } else {
            return 0;
        }
    }
    output[length] = '\0';
    return 1;
}

static int vps_safe_service_name(const char *value, size_t length)
{
    size_t index;
    if (value == NULL || length == 0U || length > 128U) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= 'A' && byte <= 'Z') ||
              (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' ||
              byte == '.')) {
            return 0;
        }
    }
    return 1;
}

static int vps_safe_service_file(const char *value, size_t length)
{
    size_t index;
    int absolute;
    if (value == NULL || length < 3U ||
        length > VPS_CREDENTIAL_VALUE_MAX_LENGTH) {
        return 0;
    }
    absolute = value[0] == '/' || value[0] == '\\' ||
               (((value[0] >= 'A' && value[0] <= 'Z') ||
                 (value[0] >= 'a' && value[0] <= 'z')) &&
                value[1] == ':' && (value[2] == '\\' || value[2] == '/'));
    if (!absolute || strstr(value, "..") != NULL || strchr(value, '%') != NULL ||
        strchr(value, '$') != NULL) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte < 0x20U || byte == 0x7fU || byte == '<' || byte == '>' ||
            byte == '"' || byte == '|' || byte == '?' || byte == '*') {
            return 0;
        }
    }
    return value[length] == '\0';
}

static VpsConnectionStringResult vps_resolve_service(
    VpsConnectionConfig *output,
    const VpsParsedArguments *arguments)
{
    VpsCredentialConfig source;
    size_t service_length;
    size_t file_length = 0U;
    const char *service = vps_argument_text(arguments, VPS_ARGUMENT_ID_SERVICE,
                                            &service_length);
    const char *file = vps_argument_text(arguments, VPS_ARGUMENT_ID_SERVICE_FILE,
                                         &file_length);
    if (!vps_safe_service_name(service, service_length) ||
        (file != NULL && !vps_safe_service_file(file, file_length))) {
        return VPS_CONNECTION_STRING_INVALID_VALUE;
    }
    (void)memset(&source, 0, sizeof(source));
    source.header.structure_size = (uint32_t)sizeof(source);
    source.header.api_version = VPS_API_VERSION;
    source.header.present_fields = VPS_CREDENTIAL_FIELD_SERVICE;
    source.service = service;
    if (file != NULL) {
        source.header.present_fields |= VPS_CREDENTIAL_FIELD_SERVICE_FILE;
        source.service_file = file;
    }
    return vps_copy_config(output, &source);
}

static const VpsConnectionField *vps_profile_field(const char *suffix)
{
    size_t index;
    for (index = 0U; index < sizeof(vps_connection_fields) /
                                  sizeof(vps_connection_fields[0]); ++index) {
        if (strcmp(vps_connection_fields[index].profile_suffix, suffix) == 0) {
            return &vps_connection_fields[index];
        }
    }
    return NULL;
}

static int vps_build_profile_name(char *output,
                                  size_t output_size,
                                  const char *profile,
                                  const char *suffix)
{
    static const char prefix[] = "VPS_PROFILE_";
    size_t prefix_length = sizeof(prefix) - 1U;
    size_t profile_length = strlen(profile);
    size_t suffix_length = suffix == NULL ? 0U : strlen(suffix);
    size_t required = prefix_length + profile_length + 1U + suffix_length + 1U;
    char *cursor;
    if (required > output_size) {
        return 0;
    }
    cursor = output;
    (void)memcpy(cursor, prefix, prefix_length);
    cursor += prefix_length;
    (void)memcpy(cursor, profile, profile_length);
    cursor += profile_length;
    *cursor++ = '_';
    if (suffix_length != 0U) {
        (void)memcpy(cursor, suffix, suffix_length);
        cursor += suffix_length;
    }
    *cursor = '\0';
    return 1;
}

static VpsConnectionStringResult vps_resolve_profile_snapshot(
    VpsConnectionConfig *output,
    const VpsConnectionResolveOptions *options)
{
    VpsCredentialConfig source;
    char prefix[VPS_PROFILE_NAME_MAX_LENGTH + 14U];
    size_t prefix_length;
    size_t index;

    (void)memset(&source, 0, sizeof(source));
    source.header.structure_size = (uint32_t)sizeof(source);
    source.header.api_version = VPS_API_VERSION;
    if (!vps_build_profile_name(prefix, sizeof(prefix),
                                output->normalized_profile, NULL)) {
        return VPS_CONNECTION_STRING_LIMIT_EXCEEDED;
    }
    prefix_length = strlen(prefix);
    for (index = 0U; index < options->profile_environment_count; ++index) {
        const VpsProfileEnvironmentEntry *entry =
            &options->profile_environment[index];
        const VpsConnectionField *field;
        size_t ignored;
        if (entry->name == NULL || entry->value == NULL ||
            strncmp(entry->name, prefix, prefix_length) != 0) {
            return VPS_CONNECTION_STRING_INVALID_VALUE;
        }
        field = vps_profile_field(entry->name + prefix_length);
        if (field == NULL || (source.header.present_fields & field->bit) != 0U ||
            !vps_value_length(entry->value, &ignored)) {
            return VPS_CONNECTION_STRING_INVALID_VALUE;
        }
        source.header.present_fields |= field->bit;
        vps_config_set(&source, field->offset, entry->value);
    }
    return vps_copy_config(output, &source);
}

static VpsConnectionStringResult vps_resolve_profile_environment(
    VpsConnectionConfig *output)
{
    VpsSensitiveMemory scratch;
    VpsCredentialConfig source;
    const VpsAllocator *allocator = &output->storage.storage.allocator;
    const VpsPlatformOperations *operations = output->storage.operations;
    size_t slot_size = VPS_CREDENTIAL_VALUE_MAX_LENGTH + 1U;
    size_t count = sizeof(vps_connection_fields) / sizeof(vps_connection_fields[0]);
    size_t total;
    size_t index;
    VpsConnectionStringResult result;

    if (vps_size_multiply(slot_size, count, &total) != VPS_MEMORY_OK ||
        vps_sensitive_memory_init(&scratch, allocator, operations,
                                  output->storage.logger) != VPS_SECURE_MEMORY_OK ||
        vps_sensitive_memory_allocate(&scratch, total) != VPS_SECURE_MEMORY_OK) {
        return VPS_CONNECTION_STRING_OUT_OF_MEMORY;
    }
    (void)memset(&source, 0, sizeof(source));
    source.header.structure_size = (uint32_t)sizeof(source);
    source.header.api_version = VPS_API_VERSION;
    for (index = 0U; index < count; ++index) {
        char name[VPS_PROFILE_NAME_MAX_LENGTH + 40U];
        char *slot = (char *)vps_sensitive_memory_data(&scratch) + index * slot_size;
        size_t required = 0U;
        VpsPlatformStatus status;
        if (!vps_build_profile_name(name, sizeof(name),
                output->normalized_profile,
                vps_connection_fields[index].profile_suffix)) {
            (void)vps_sensitive_memory_release(&scratch);
            return VPS_CONNECTION_STRING_LIMIT_EXCEEDED;
        }
        status = vps_platform_environment_get(operations, name, slot, slot_size,
                                              &required);
        if (status == VPS_PLATFORM_NOT_FOUND) {
            continue;
        }
        if (status == VPS_PLATFORM_BUFFER_TOO_SMALL || required > slot_size) {
            (void)vps_sensitive_memory_release(&scratch);
            return VPS_CONNECTION_STRING_LIMIT_EXCEEDED;
        }
        if (status != VPS_PLATFORM_OK || required == 0U ||
            slot[required - 1U] != '\0') {
            (void)vps_sensitive_memory_release(&scratch);
            return VPS_CONNECTION_STRING_ENVIRONMENT_ERROR;
        }
        source.header.present_fields |= vps_connection_fields[index].bit;
        vps_config_set(&source, vps_connection_fields[index].offset, slot);
    }
    result = vps_copy_config(output, &source);
    if (vps_sensitive_memory_release(&scratch) != VPS_SECURE_MEMORY_OK) {
        return VPS_CONNECTION_STRING_CLEANUP_FAILED;
    }
    return result;
}

static VpsConnectionStringResult vps_resolve_profile(
    VpsConnectionConfig *output,
    const VpsParsedArguments *arguments,
    const VpsConnectionResolveOptions *options)
{
    size_t length;
    const char *profile = vps_argument_text(arguments, VPS_ARGUMENT_ID_PROFILE,
                                            &length);
    if (!vps_normalize_profile(profile, length, output->normalized_profile)) {
        return VPS_CONNECTION_STRING_INVALID_VALUE;
    }
    if (options->profile_environment != NULL ||
        options->profile_environment_count != 0U) {
        if (options->profile_environment == NULL ||
            options->profile_environment_count == 0U) {
            return VPS_CONNECTION_STRING_INVALID_ARGUMENT;
        }
        return vps_resolve_profile_snapshot(output, options);
    }
    return vps_resolve_profile_environment(output);
}

static VpsConnectionStringResult vps_resolve_reference(
    VpsConnectionConfig *output,
    const VpsParsedArguments *arguments,
    VpsCredentialRegistry *registry)
{
    VpsResolvedCredential resolved;
    VpsCredentialRegistryResult provider_result;
    VpsConnectionStringResult result;
    size_t length;
    uint32_t length32;
    const char *reference = vps_argument_text(
        arguments, VPS_ARGUMENT_ID_CREDENTIAL_REF, &length);

    if (registry == NULL) {
        return VPS_CONNECTION_STRING_PROVIDER_UNAVAILABLE;
    }
    if (vps_size_to_uint32(length, &length32) != VPS_MEMORY_OK ||
        vps_resolved_credential_init(&resolved,
            &output->storage.storage.allocator, output->storage.operations,
            output->storage.logger) != VPS_CREDENTIAL_REGISTRY_OK) {
        return VPS_CONNECTION_STRING_INVALID_VALUE;
    }
    provider_result = vps_credential_registry_resolve(registry, reference,
                                                      length32, &resolved);
    if (provider_result != VPS_CREDENTIAL_REGISTRY_OK) {
        (void)vps_resolved_credential_cleanup(&resolved);
        return provider_result == VPS_CREDENTIAL_REGISTRY_NOT_REGISTERED
                   ? VPS_CONNECTION_STRING_PROVIDER_UNAVAILABLE
                   : VPS_CONNECTION_STRING_RESOLVE_FAILED;
    }
    result = vps_copy_config(output, &resolved.config);
    output->provider_id = resolved.provider_id;
    output->generation = resolved.generation;
    if (vps_resolved_credential_cleanup(&resolved) !=
        VPS_CREDENTIAL_REGISTRY_OK) {
        return VPS_CONNECTION_STRING_CLEANUP_FAILED;
    }
    return result;
}

static void vps_connection_log(VpsConnectionConfig *connection,
                               VpsConnectionStringResult result)
{
    static const char operation[] = "connection_config";
    VpsLogEvent event;
    const char *mode = vps_connection_mode_name(connection->mode);
    const char *status = vps_connection_string_result_name(result);
    VpsLogLevel level = result == VPS_CONNECTION_STRING_OK
                            ? VPS_LOG_LEVEL_INFO
                            : VPS_LOG_LEVEL_WARN;
    if (connection->storage.logger == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, mode,
                                 strlen(mode)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PRESENCE_MASK,
            connection->config.header.present_fields) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK) {
        return;
    }
    if (connection->mode == VPS_CONNECTION_MODE_PROFILE &&
        connection->normalized_profile[0] != '\0') {
        (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_PROFILE,
            connection->normalized_profile, strlen(connection->normalized_profile));
    } else if (connection->mode == VPS_CONNECTION_MODE_SERVICE &&
               connection->config.service != NULL) {
        (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_SERVICE,
            connection->config.service, strlen(connection->config.service));
    } else if (connection->mode == VPS_CONNECTION_MODE_CREDENTIAL_REF) {
        (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PROVIDER_ID,
                                      connection->provider_id);
        (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_GENERATION,
                                      connection->generation);
    }
    vps_logger_emit(connection->storage.logger, &event);
}

VpsConnectionStringResult vps_connection_config_init(
    VpsConnectionConfig *connection,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger)
{
    if (connection == NULL) {
        return VPS_CONNECTION_STRING_INVALID_ARGUMENT;
    }
    (void)memset(connection, 0, sizeof(*connection));
    if (vps_sensitive_memory_init(&connection->storage, allocator, operations,
                                  logger) != VPS_SECURE_MEMORY_OK) {
        return VPS_CONNECTION_STRING_INVALID_ARGUMENT;
    }
    connection->config.header.structure_size =
        (uint32_t)sizeof(connection->config);
    connection->config.header.api_version = VPS_API_VERSION;
    connection->initialized = 1;
    return VPS_CONNECTION_STRING_OK;
}

VpsConnectionStringResult vps_connection_config_resolve(
    VpsConnectionConfig *connection,
    const VpsParsedArguments *arguments,
    const VpsConnectionResolveOptions *options)
{
    VpsConnectionConfig replacement;
    VpsConnectionStringResult result;
    VpsArgumentMask modes;
    size_t length;
    const char *conninfo;

    if (connection == NULL || !connection->initialized || arguments == NULL ||
        !arguments->initialized || options == NULL ||
        vps_connection_config_init(&replacement,
            &connection->storage.storage.allocator, connection->storage.operations,
            connection->storage.logger) != VPS_CONNECTION_STRING_OK) {
        return VPS_CONNECTION_STRING_INVALID_ARGUMENT;
    }
    modes = arguments->presence & VPS_ARGUMENT_CONNECTION_MODES;
    if (modes == 0U || (modes & (modes - 1U)) != 0U) {
        (void)vps_connection_config_cleanup(&replacement);
        return VPS_CONNECTION_STRING_INVALID_MODE;
    }
    if ((modes & VPS_ARGUMENT_CREDENTIAL_REF) != 0U) {
        replacement.mode = VPS_CONNECTION_MODE_CREDENTIAL_REF;
        result = vps_resolve_reference(&replacement, arguments,
                                       options->credential_registry);
    } else if ((modes & VPS_ARGUMENT_SERVICE) != 0U) {
        replacement.mode = VPS_CONNECTION_MODE_SERVICE;
        result = vps_resolve_service(&replacement, arguments);
    } else if ((modes & VPS_ARGUMENT_PROFILE) != 0U) {
        replacement.mode = VPS_CONNECTION_MODE_PROFILE;
        result = vps_resolve_profile(&replacement, arguments, options);
    } else {
        replacement.mode = VPS_CONNECTION_MODE_CONNSTR;
        replacement.persistent_connstr_risk = 1;
        conninfo = vps_argument_text(arguments, VPS_ARGUMENT_ID_CONNSTR, &length);
        result = options->conninfo_parser == NULL ||
                         options->conninfo_parser->parse == NULL
                     ? VPS_CONNECTION_STRING_INVALID_ARGUMENT
                     : options->conninfo_parser->parse(
                           options->conninfo_parser->context, conninfo, length,
                           vps_consume_conninfo, &replacement);
    }
    vps_connection_log(&replacement, result);
    if (result != VPS_CONNECTION_STRING_OK) {
        (void)vps_connection_config_cleanup(&replacement);
        return result;
    }
    if (vps_sensitive_memory_release(&connection->storage) !=
        VPS_SECURE_MEMORY_OK) {
        (void)vps_connection_config_cleanup(&replacement);
        return VPS_CONNECTION_STRING_CLEANUP_FAILED;
    }
    *connection = replacement;
    return VPS_CONNECTION_STRING_OK;
}

VpsConnectionStringResult vps_connection_config_cleanup(
    VpsConnectionConfig *connection)
{
    if (connection == NULL) {
        return VPS_CONNECTION_STRING_INVALID_ARGUMENT;
    }
    if (!connection->initialized) {
        return VPS_CONNECTION_STRING_OK;
    }
    if (vps_sensitive_memory_release(&connection->storage) !=
        VPS_SECURE_MEMORY_OK) {
        return VPS_CONNECTION_STRING_CLEANUP_FAILED;
    }
    (void)memset(connection, 0, sizeof(*connection));
    return VPS_CONNECTION_STRING_OK;
}

const char *vps_connection_mode_name(VpsConnectionMode mode)
{
    switch (mode) {
    case VPS_CONNECTION_MODE_CREDENTIAL_REF: return "credential_ref";
    case VPS_CONNECTION_MODE_SERVICE: return "service";
    case VPS_CONNECTION_MODE_PROFILE: return "profile";
    case VPS_CONNECTION_MODE_CONNSTR: return "connstr";
    default: return "none";
    }
}

const char *vps_connection_string_result_name(VpsConnectionStringResult result)
{
    switch (result) {
    case VPS_CONNECTION_STRING_OK: return "ok";
    case VPS_CONNECTION_STRING_INVALID_ARGUMENT: return "invalid_argument";
    case VPS_CONNECTION_STRING_INVALID_MODE: return "invalid_mode";
    case VPS_CONNECTION_STRING_INVALID_VALUE: return "invalid_value";
    case VPS_CONNECTION_STRING_LIMIT_EXCEEDED: return "limit_exceeded";
    case VPS_CONNECTION_STRING_PROVIDER_UNAVAILABLE: return "provider_unavailable";
    case VPS_CONNECTION_STRING_RESOLVE_FAILED: return "resolve_failed";
    case VPS_CONNECTION_STRING_ENVIRONMENT_ERROR: return "environment_error";
    case VPS_CONNECTION_STRING_CONNINFO_REJECTED: return "conninfo_rejected";
    case VPS_CONNECTION_STRING_OUT_OF_MEMORY: return "out_of_memory";
    case VPS_CONNECTION_STRING_CLEANUP_FAILED: return "cleanup_failed";
    default: return "unknown";
    }
}
