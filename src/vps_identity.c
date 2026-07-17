#include "vps_identity.h"

#include <stddef.h>
#include <string.h>

typedef enum VpsIdentityField {
    VPS_IDENTITY_FIELD_BACKEND = 1,
    VPS_IDENTITY_FIELD_HOSTS = 2,
    VPS_IDENTITY_FIELD_PORTS = 3,
    VPS_IDENTITY_FIELD_DBNAME = 4,
    VPS_IDENTITY_FIELD_USER = 5,
    VPS_IDENTITY_FIELD_SERVICE = 6,
    VPS_IDENTITY_FIELD_SERVICE_FILE = 7,
    VPS_IDENTITY_FIELD_SSLMODE = 8,
    VPS_IDENTITY_FIELD_SSLROOTCERT = 9,
    VPS_IDENTITY_FIELD_SSLCERT = 10,
    VPS_IDENTITY_FIELD_SSLKEY = 11,
    VPS_IDENTITY_FIELD_SSLCRL = 12,
    VPS_IDENTITY_FIELD_CHANNEL_BINDING = 13,
    VPS_IDENTITY_FIELD_TARGET_SESSION_ATTRS = 14,
    VPS_IDENTITY_FIELD_SEARCH_PATH = 15,
    VPS_IDENTITY_FIELD_READ_WRITE_CLASS = 16,
    VPS_IDENTITY_FIELD_CONNECT_TIMEOUT = 17,
    VPS_IDENTITY_FIELD_STATEMENT_TIMEOUT = 18,
    VPS_IDENTITY_FIELD_LOCK_TIMEOUT = 19
} VpsIdentityField;

static const unsigned char vps_identity_header[] = {'V', 'P', 'S', 'I', 1U};
static const char vps_default_backend[] = "postgresql";
static const char vps_default_port[] = "5432";
static const char vps_default_sslmode[] = "verify-full";
static const char vps_default_channel_binding[] = "prefer";
static const char vps_default_target_session_attrs[] = "any";
static const char vps_default_read_only[] = "ro";
static const char vps_read_write[] = "rw";

static int vps_identity_bounded_length(const char *value, size_t *length)
{
    size_t index;
    if (value == NULL || length == NULL) {
        return 0;
    }
    for (index = 0U; index <= VPS_CREDENTIAL_VALUE_MAX_LENGTH; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte == 0U) {
            *length = index;
            return 1;
        }
        if (byte < 0x20U || byte == 0x7fU) {
            return 0;
        }
    }
    return 0;
}

static VpsIdentityResult vps_identity_append(VpsBuffer *buffer,
                                             VpsIdentityField field,
                                             const char *value,
                                             size_t length)
{
    unsigned char header[5];
    VpsMemoryResult result;
    if (length > UINT32_MAX) {
        return VPS_IDENTITY_LIMIT_EXCEEDED;
    }
    header[0] = (unsigned char)field;
    header[1] = (unsigned char)(length & 0xffU);
    header[2] = (unsigned char)((length >> 8) & 0xffU);
    header[3] = (unsigned char)((length >> 16) & 0xffU);
    header[4] = (unsigned char)((length >> 24) & 0xffU);
    result = vps_buffer_append(buffer, header, sizeof(header));
    if (result == VPS_MEMORY_OK && length != 0U) {
        result = vps_buffer_append(buffer, value, length);
    }
    if (result == VPS_MEMORY_LIMIT_EXCEEDED || result == VPS_MEMORY_OVERFLOW) {
        return VPS_IDENTITY_LIMIT_EXCEEDED;
    }
    return result == VPS_MEMORY_OK ? VPS_IDENTITY_OK
                                   : VPS_IDENTITY_OUT_OF_MEMORY;
}

static char vps_ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int vps_identity_copy_normalized(const char *value,
                                        size_t length,
                                        int lower_ascii,
                                        char *output,
                                        size_t output_size,
                                        size_t *output_length)
{
    size_t begin = 0U;
    size_t end = length;
    size_t index;
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (value[end - 1U] == ' ' || value[end - 1U] == '\t')) {
        --end;
    }
    if (end - begin >= output_size) {
        return 0;
    }
    for (index = begin; index < end; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte < 0x20U || byte == 0x7fU) {
            return 0;
        }
        output[index - begin] = lower_ascii ? vps_ascii_lower(value[index])
                                            : value[index];
    }
    *output_length = end - begin;
    output[*output_length] = '\0';
    return 1;
}

static int vps_identity_normalize_list(const char *value,
                                       size_t length,
                                       int lower_ascii,
                                       char *output,
                                       size_t output_size,
                                       size_t *output_length)
{
    size_t input = 0U;
    size_t written = 0U;
    size_t item_index = 0U;
    while (input <= length) {
        size_t end = input;
        size_t content_begin;
        size_t item_length;
        char item[VPS_CREDENTIAL_VALUE_MAX_LENGTH + 1U];
        while (end < length && value[end] != ',') {
            ++end;
        }
        content_begin = input;
        while (content_begin < end &&
               (value[content_begin] == ' ' || value[content_begin] == '\t')) {
            ++content_begin;
        }
        if (!vps_identity_copy_normalized(value + input, end - input,
                                          lower_ascii && content_begin != end &&
                                              value[content_begin] != '/' &&
                                              value[content_begin] != '\\',
                                          item, sizeof(item),
                                          &item_length) ||
            written + item_length + (item_index == 0U ? 0U : 1U) >= output_size) {
            return 0;
        }
        if (item_index != 0U) {
            output[written++] = ',';
        }
        if (item_length != 0U) {
            (void)memcpy(output + written, item, item_length);
            written += item_length;
        }
        ++item_index;
        if (end == length) {
            break;
        }
        input = end + 1U;
    }
    output[written] = '\0';
    *output_length = written;
    return 1;
}

static int vps_identity_parse_port(const char *value,
                                   size_t length,
                                   uint32_t *port)
{
    size_t index;
    uint32_t parsed = 0U;
    if (length == 0U) {
        *port = 5432U;
        return 1;
    }
    for (index = 0U; index < length; ++index) {
        uint32_t digit;
        if (value[index] < '0' || value[index] > '9') {
            return 0;
        }
        digit = (uint32_t)(value[index] - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return 0;
        }
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0U || parsed > 65535U) {
        return 0;
    }
    *port = parsed;
    return 1;
}

static size_t vps_identity_write_uint32(char *output, uint32_t value)
{
    char reversed[10];
    size_t count = 0U;
    size_t index;
    do {
        reversed[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    for (index = 0U; index < count; ++index) {
        output[index] = reversed[count - index - 1U];
    }
    output[count] = '\0';
    return count;
}

static int vps_identity_normalize_ports(const char *value,
                                        size_t length,
                                        char *output,
                                        size_t output_size,
                                        size_t *output_length)
{
    size_t input = 0U;
    size_t written = 0U;
    size_t item_index = 0U;
    while (input <= length) {
        size_t end = input;
        size_t begin;
        uint32_t port;
        char decimal[11];
        size_t decimal_length;
        while (end < length && value[end] != ',') ++end;
        begin = input;
        while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
        while (end > begin && (value[end - 1U] == ' ' || value[end - 1U] == '\t')) --end;
        if (!vps_identity_parse_port(value + begin, end - begin, &port)) return 0;
        decimal_length = vps_identity_write_uint32(decimal, port);
        if (written + decimal_length + (item_index == 0U ? 0U : 1U) >= output_size) return 0;
        if (item_index != 0U) output[written++] = ',';
        (void)memcpy(output + written, decimal, decimal_length);
        written += decimal_length;
        ++item_index;
        end = input;
        while (end < length && value[end] != ',') ++end;
        if (end == length) break;
        input = end + 1U;
    }
    output[written] = '\0';
    *output_length = written;
    return 1;
}

static int vps_identity_normalize_path(const char *value,
                                       size_t length,
                                       char *output,
                                       size_t output_size,
                                       size_t *output_length)
{
    size_t segment_offsets[VPS_CREDENTIAL_VALUE_MAX_LENGTH / 2U + 2U];
    unsigned char segment_is_parent[
        VPS_CREDENTIAL_VALUE_MAX_LENGTH / 2U + 2U] = {0};
    size_t segment_count = 0U;
    size_t input = 0U;
    size_t written = 0U;
    int absolute = length != 0U && (value[0] == '/' || value[0] == '\\');
    int network = length >= 2U &&
        (value[0] == '/' || value[0] == '\\') &&
        (value[1] == '/' || value[1] == '\\');
    int drive = length >= 2U &&
        ((value[0] >= 'A' && value[0] <= 'Z') ||
         (value[0] >= 'a' && value[0] <= 'z')) && value[1] == ':';
    if (output_size == 0U) return 0;
    if (drive) {
        if (output_size < 3U) return 0;
        output[written++] = vps_ascii_lower(value[0]);
        output[written++] = ':';
        input = 2U;
        if (input < length && (value[input] == '/' || value[input] == '\\')) {
            output[written++] = '/';
            ++input;
            absolute = 1;
        }
    } else if (absolute) {
        output[written++] = '/';
        if (network) output[written++] = '/';
        while (input < length && (value[input] == '/' || value[input] == '\\')) ++input;
    }
    while (input <= length) {
        size_t end = input;
        size_t segment_length;
        while (end < length && value[end] != '/' && value[end] != '\\') ++end;
        segment_length = end - input;
        if (segment_length == 0U ||
            (segment_length == 1U && value[input] == '.')) {
            /* Skip empty and current-directory segments. */
        } else if (segment_length == 2U && value[input] == '.' &&
                   value[input + 1U] == '.') {
            if (segment_count != 0U &&
                segment_is_parent[segment_count - 1U] == 0U) {
                written = segment_offsets[--segment_count];
            } else if (!absolute) {
                if (written != 0U && output[written - 1U] != '/') output[written++] = '/';
                if (written + 2U >= output_size) return 0;
                segment_offsets[segment_count++] = written;
                segment_is_parent[segment_count - 1U] = 1U;
                output[written++] = '.';
                output[written++] = '.';
            }
        } else {
            size_t rollback = written;
            if (written != 0U && output[written - 1U] != '/') output[written++] = '/';
            if (written + segment_length >= output_size ||
                segment_count >= sizeof(segment_offsets) / sizeof(segment_offsets[0])) return 0;
            segment_offsets[segment_count++] = rollback;
            segment_is_parent[segment_count - 1U] = 0U;
            (void)memcpy(output + written, value + input, segment_length);
            written += segment_length;
        }
        if (end == length) break;
        input = end + 1U;
        while (input < length && (value[input] == '/' || value[input] == '\\')) ++input;
    }
    output[written] = '\0';
    *output_length = written;
    return 1;
}

static VpsIdentityResult vps_identity_add_text(VpsBuffer *buffer,
                                               VpsIdentityField field,
                                               const char *value,
                                               const char *default_value,
                                               int lower_ascii)
{
    char normalized[VPS_CREDENTIAL_VALUE_MAX_LENGTH + 1U];
    size_t length;
    size_t normalized_length;
    if (value == NULL) value = default_value;
    if (value == NULL || !vps_identity_bounded_length(value, &length) ||
        !vps_identity_copy_normalized(value, length, lower_ascii, normalized,
                                      sizeof(normalized), &normalized_length)) {
        return VPS_IDENTITY_INVALID_VALUE;
    }
    return vps_identity_append(buffer, field, normalized, normalized_length);
}

static VpsIdentityResult vps_identity_add_list(VpsBuffer *buffer,
                                               VpsIdentityField field,
                                               const char *value,
                                               const char *default_value,
                                               int ports,
                                               int lower_ascii)
{
    char normalized[VPS_CREDENTIAL_VALUE_MAX_LENGTH + 1U];
    size_t length;
    size_t normalized_length;
    if (value == NULL) value = default_value;
    if (value == NULL || !vps_identity_bounded_length(value, &length) ||
        !(ports ? vps_identity_normalize_ports(value, length, normalized,
                                               sizeof(normalized), &normalized_length)
                 : vps_identity_normalize_list(value, length, lower_ascii,
                                               normalized, sizeof(normalized),
                                               &normalized_length))) {
        return VPS_IDENTITY_INVALID_VALUE;
    }
    return vps_identity_append(buffer, field, normalized, normalized_length);
}

static VpsIdentityResult vps_identity_add_path(VpsBuffer *buffer,
                                               VpsIdentityField field,
                                               const char *value)
{
    char normalized[VPS_CREDENTIAL_VALUE_MAX_LENGTH + 1U];
    size_t length;
    size_t normalized_length;
    if (value == NULL) return vps_identity_append(buffer, field, "", 0U);
    if (!vps_identity_bounded_length(value, &length) ||
        !vps_identity_normalize_path(value, length, normalized,
                                     sizeof(normalized), &normalized_length)) {
        return VPS_IDENTITY_INVALID_VALUE;
    }
    return vps_identity_append(buffer, field, normalized, normalized_length);
}

static int vps_identity_normalize_timeout(
    const VpsParsedArguments *arguments,
    VpsArgumentId argument_id,
    const char *config_value,
    char decimal[11])
{
    const VpsArgumentValue *argument = vps_arguments_get(arguments, argument_id);
    size_t length;
    size_t begin = 0U;
    size_t end;
    size_t index;
    uint32_t parsed = 0U;
    if (argument != NULL && argument->present) {
        (void)vps_identity_write_uint32(decimal, argument->uint32_value);
        return 1;
    }
    if (config_value == NULL) {
        decimal[0] = '0';
        decimal[1] = '\0';
        return 1;
    }
    if (!vps_identity_bounded_length(config_value, &length)) return 0;
    end = length;
    while (begin < end && (config_value[begin] == ' ' || config_value[begin] == '\t')) ++begin;
    while (end > begin && (config_value[end - 1U] == ' ' || config_value[end - 1U] == '\t')) --end;
    if (begin == end) return 0;
    for (index = begin; index < end; ++index) {
        uint32_t digit;
        if (config_value[index] < '0' || config_value[index] > '9') return 0;
        digit = (uint32_t)(config_value[index] - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) return 0;
        parsed = parsed * 10U + digit;
    }
    (void)vps_identity_write_uint32(decimal, parsed);
    return 1;
}

static void vps_identity_hash(const unsigned char *data,
                              size_t length,
                              char output[VPS_IDENTITY_FINGERPRINT_BUFFER_SIZE])
{
    static const uint64_t seeds[4] = {
        UINT64_C(14695981039346656037), UINT64_C(7809847782465536322),
        UINT64_C(9650029242287828579), UINT64_C(2870177450012600261)};
    static const char hex[] = "0123456789abcdef";
    uint64_t hashes[4];
    size_t hash_index;
    size_t index;
    for (hash_index = 0U; hash_index < 4U; ++hash_index) {
        hashes[hash_index] = seeds[hash_index];
        for (index = 0U; index < length; ++index) {
            hashes[hash_index] ^= (uint64_t)data[index] + hash_index;
            hashes[hash_index] *= UINT64_C(1099511628211);
        }
        hashes[hash_index] ^= hashes[hash_index] >> 32;
    }
    for (hash_index = 0U; hash_index < 4U; ++hash_index) {
        for (index = 0U; index < 16U; ++index) {
            unsigned int shift = (unsigned int)((15U - index) * 4U);
            output[hash_index * 16U + index] =
                hex[(hashes[hash_index] >> shift) & 0x0fU];
        }
    }
    output[VPS_IDENTITY_FINGERPRINT_LENGTH] = '\0';
}

static void vps_identity_log(const VpsConnectionIdentity *identity,
                             const char *phase,
                             const char *status,
                             int include_fingerprint)
{
    static const char operation[] = "connection_identity";
    VpsLogEvent event;
    if (identity == NULL || identity->logger == NULL ||
        vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 strlen(phase)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_GENERATION,
                                 identity->credential_generation) !=
            VPS_LOG_OK ||
        vps_log_event_add_uint64(&event,
                                 VPS_LOG_FIELD_CONFIGURATION_GENERATION,
                                 identity->configuration_generation) !=
            VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK) return;
    if (include_fingerprint) {
        (void)vps_log_event_add_string(&event,
            VPS_LOG_FIELD_CONNECTION_FINGERPRINT, identity->fingerprint,
            VPS_IDENTITY_FINGERPRINT_LENGTH);
    }
    vps_logger_emit(identity->logger, &event);
}

VpsIdentityResult vps_identity_init(VpsConnectionIdentity *identity,
                                    const VpsAllocator *allocator,
                                    VpsLogger *logger)
{
    if (identity == NULL) return VPS_IDENTITY_INVALID_ARGUMENT;
    (void)memset(identity, 0, sizeof(*identity));
    if (vps_buffer_init(&identity->canonical, allocator,
                        VPS_IDENTITY_CANONICAL_LIMIT) != VPS_MEMORY_OK) {
        return VPS_IDENTITY_INVALID_ARGUMENT;
    }
    identity->logger = logger;
    identity->initialized = 1;
    return VPS_IDENTITY_OK;
}

VpsIdentityResult vps_identity_build(
    VpsConnectionIdentity *identity,
    const VpsConnectionConfig *connection,
    const VpsParsedArguments *arguments,
    const VpsIdentityBuildOptions *options)
{
    VpsConnectionIdentity replacement;
    const VpsCredentialConfig *config;
    const VpsArgumentValue *mode;
    const char *backend;
    size_t backend_length;
    char connect_timeout[11];
    char statement_timeout[11];
    char lock_timeout[11];
    VpsIdentityResult result;
#define VPS_IDENTITY_TRY(expression) do { result = (expression); if (result != VPS_IDENTITY_OK) goto failure; } while (0)
    if (identity == NULL || !identity->initialized || connection == NULL ||
        !connection->initialized || arguments == NULL || !arguments->initialized ||
        options == NULL || vps_identity_init(&replacement,
            &identity->canonical.allocator, identity->logger) != VPS_IDENTITY_OK) {
        return VPS_IDENTITY_INVALID_ARGUMENT;
    }
    config = &connection->config;
    if (vps_buffer_append(&replacement.canonical, vps_identity_header,
                          sizeof(vps_identity_header)) != VPS_MEMORY_OK) {
        result = VPS_IDENTITY_OUT_OF_MEMORY;
        goto failure;
    }
    backend = options->backend_name == NULL ? vps_default_backend
                                            : options->backend_name;
    backend_length = options->backend_name == NULL
                         ? sizeof(vps_default_backend) - 1U
                         : options->backend_name_length;
    if (backend_length == 0U || backend_length > 64U ||
        memchr(backend, '\0', backend_length) != NULL) {
        result = VPS_IDENTITY_INVALID_VALUE;
        goto failure;
    }
    {
        char normalized_backend[65];
        size_t normalized_length;
        if (!vps_identity_copy_normalized(backend, backend_length, 1,
                normalized_backend, sizeof(normalized_backend), &normalized_length)) {
            result = VPS_IDENTITY_INVALID_VALUE;
            goto failure;
        }
        VPS_IDENTITY_TRY(vps_identity_append(&replacement.canonical,
            VPS_IDENTITY_FIELD_BACKEND, normalized_backend, normalized_length));
    }
    VPS_IDENTITY_TRY(vps_identity_add_list(&replacement.canonical,
        VPS_IDENTITY_FIELD_HOSTS, config->hosts, "", 0, 1));
    VPS_IDENTITY_TRY(vps_identity_add_list(&replacement.canonical,
        VPS_IDENTITY_FIELD_PORTS, config->ports,
        config->hosts == NULL ? "" : vps_default_port, 1, 0));
    VPS_IDENTITY_TRY(vps_identity_add_text(&replacement.canonical,
        VPS_IDENTITY_FIELD_DBNAME, config->dbname, "", 0));
    VPS_IDENTITY_TRY(vps_identity_add_text(&replacement.canonical,
        VPS_IDENTITY_FIELD_USER, config->user, "", 0));
    VPS_IDENTITY_TRY(vps_identity_add_text(&replacement.canonical,
        VPS_IDENTITY_FIELD_SERVICE, config->service, "", 0));
    VPS_IDENTITY_TRY(vps_identity_add_path(&replacement.canonical,
        VPS_IDENTITY_FIELD_SERVICE_FILE, config->service_file));
    VPS_IDENTITY_TRY(vps_identity_add_text(&replacement.canonical,
        VPS_IDENTITY_FIELD_SSLMODE, config->sslmode, vps_default_sslmode, 1));
    VPS_IDENTITY_TRY(vps_identity_add_path(&replacement.canonical,
        VPS_IDENTITY_FIELD_SSLROOTCERT, config->sslrootcert));
    VPS_IDENTITY_TRY(vps_identity_add_path(&replacement.canonical,
        VPS_IDENTITY_FIELD_SSLCERT, config->sslcert));
    VPS_IDENTITY_TRY(vps_identity_add_path(&replacement.canonical,
        VPS_IDENTITY_FIELD_SSLKEY, config->sslkey));
    VPS_IDENTITY_TRY(vps_identity_add_path(&replacement.canonical,
        VPS_IDENTITY_FIELD_SSLCRL, config->sslcrl));
    VPS_IDENTITY_TRY(vps_identity_add_text(&replacement.canonical,
        VPS_IDENTITY_FIELD_CHANNEL_BINDING, config->channel_binding,
        vps_default_channel_binding, 1));
    VPS_IDENTITY_TRY(vps_identity_add_text(&replacement.canonical,
        VPS_IDENTITY_FIELD_TARGET_SESSION_ATTRS, config->target_session_attrs,
        vps_default_target_session_attrs, 1));
    VPS_IDENTITY_TRY(vps_identity_add_list(&replacement.canonical,
        VPS_IDENTITY_FIELD_SEARCH_PATH, config->search_path, "", 0, 0));
    mode = vps_arguments_get(arguments, VPS_ARGUMENT_ID_MODE);
    VPS_IDENTITY_TRY(vps_identity_add_text(&replacement.canonical,
        VPS_IDENTITY_FIELD_READ_WRITE_CLASS,
        mode != NULL && mode->present &&
                mode->enum_value == VPS_ARGUMENT_ENUM_MODE_RW
            ? vps_read_write : vps_default_read_only, NULL, 1));
    if (!vps_identity_normalize_timeout(arguments, VPS_ARGUMENT_ID_CONNECT_TIMEOUT,
                                        config->connect_timeout, connect_timeout) ||
        !vps_identity_normalize_timeout(arguments, VPS_ARGUMENT_ID_STATEMENT_TIMEOUT,
                                        config->statement_timeout, statement_timeout) ||
        !vps_identity_normalize_timeout(arguments, VPS_ARGUMENT_ID_LOCK_TIMEOUT,
                                        config->lock_timeout, lock_timeout)) {
        result = VPS_IDENTITY_INVALID_VALUE;
        goto failure;
    }
    VPS_IDENTITY_TRY(vps_identity_append(&replacement.canonical,
        VPS_IDENTITY_FIELD_CONNECT_TIMEOUT, connect_timeout, strlen(connect_timeout)));
    VPS_IDENTITY_TRY(vps_identity_append(&replacement.canonical,
        VPS_IDENTITY_FIELD_STATEMENT_TIMEOUT, statement_timeout, strlen(statement_timeout)));
    VPS_IDENTITY_TRY(vps_identity_append(&replacement.canonical,
        VPS_IDENTITY_FIELD_LOCK_TIMEOUT, lock_timeout, strlen(lock_timeout)));
    replacement.credential_generation = options->credential_generation != 0U
        ? options->credential_generation : connection->generation;
    replacement.configuration_generation = options->configuration_generation;
    vps_identity_hash(replacement.canonical.data, replacement.canonical.size,
                      replacement.fingerprint);
    replacement.built = 1;
    vps_identity_log(&replacement, "build", "ok", 1);
    vps_buffer_reset(&identity->canonical);
    *identity = replacement;
    return VPS_IDENTITY_OK;

failure:
    vps_identity_log(&replacement, "build", vps_identity_result_name(result), 0);
    vps_identity_cleanup(&replacement);
    return result;
#undef VPS_IDENTITY_TRY
}

void vps_identity_cleanup(VpsConnectionIdentity *identity)
{
    if (identity == NULL || !identity->initialized) return;
    vps_buffer_reset(&identity->canonical);
    (void)memset(identity, 0, sizeof(*identity));
}

VpsIdentityComparison vps_identity_compare(
    const VpsConnectionIdentity *left,
    const VpsConnectionIdentity *right)
{
    VpsIdentityComparison comparison = VPS_IDENTITY_DIFFERENT;
    if (left != NULL && right != NULL && left->initialized && right->initialized &&
        left->built && right->built &&
        left->canonical.size == right->canonical.size &&
        (left->canonical.size == 0U ||
         memcmp(left->canonical.data, right->canonical.data,
                left->canonical.size) == 0)) {
        comparison = left->credential_generation == right->credential_generation &&
                     left->configuration_generation == right->configuration_generation
                         ? VPS_IDENTITY_SAME
                         : VPS_IDENTITY_GENERATION_CHANGED;
    }
    if (left != NULL && left->initialized && left->built) {
        vps_identity_log(left, "compare",
                         vps_identity_comparison_name(comparison), 1);
    }
    return comparison;
}

const char *vps_identity_fingerprint(const VpsConnectionIdentity *identity)
{
    return identity != NULL && identity->initialized && identity->built &&
                   identity->fingerprint[0] != '\0'
               ? identity->fingerprint : NULL;
}

const char *vps_identity_result_name(VpsIdentityResult result)
{
    switch (result) {
    case VPS_IDENTITY_OK: return "ok";
    case VPS_IDENTITY_INVALID_ARGUMENT: return "invalid_argument";
    case VPS_IDENTITY_INVALID_VALUE: return "invalid_value";
    case VPS_IDENTITY_LIMIT_EXCEEDED: return "limit_exceeded";
    case VPS_IDENTITY_OUT_OF_MEMORY: return "out_of_memory";
    default: return "unknown";
    }
}

const char *vps_identity_comparison_name(VpsIdentityComparison comparison)
{
    switch (comparison) {
    case VPS_IDENTITY_DIFFERENT: return "different";
    case VPS_IDENTITY_SAME: return "same";
    case VPS_IDENTITY_GENERATION_CHANGED: return "generation_changed";
    default: return "unknown";
    }
}
