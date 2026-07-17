#include "vps_arguments.h"

#include <string.h>

typedef struct VpsArgumentDefinition {
    const char *name;
    VpsArgumentType type;
    size_t limit;
    int sensitive;
} VpsArgumentDefinition;

static const VpsArgumentDefinition vps_argument_definitions[] = {
    {"credential_ref", VPS_ARGUMENT_TYPE_STRING, 255U, 0},
    {"service", VPS_ARGUMENT_TYPE_STRING, 255U, 0},
    {"service_file", VPS_ARGUMENT_TYPE_STRING, 1024U, 0},
    {"profile", VPS_ARGUMENT_TYPE_STRING, 255U, 0},
    {"connstr", VPS_ARGUMENT_TYPE_STRING, 4096U, 1},
    {"source", VPS_ARGUMENT_TYPE_ENUM, 16U, 0},
    {"schema", VPS_ARGUMENT_TYPE_STRING, 255U, 0},
    {"table", VPS_ARGUMENT_TYPE_STRING, 255U, 0},
    {"query", VPS_ARGUMENT_TYPE_STRING, VPS_ARGUMENT_VALUE_LIMIT, 1},
    {"query_profile", VPS_ARGUMENT_TYPE_STRING, 255U, 0},
    {"mode", VPS_ARGUMENT_TYPE_ENUM, 8U, 0},
    {"allow_view", VPS_ARGUMENT_TYPE_BOOLEAN, 5U, 0},
    {"allow_materialized_view", VPS_ARGUMENT_TYPE_BOOLEAN, 5U, 0},
    {"allow_foreign_table", VPS_ARGUMENT_TYPE_BOOLEAN, 5U, 0},
    {"key_columns", VPS_ARGUMENT_TYPE_STRING, 2048U, 0},
    {"optimistic_lock", VPS_ARGUMENT_TYPE_STRING, 255U, 0},
    {"version_column", VPS_ARGUMENT_TYPE_STRING, 255U, 0},
    {"geometry", VPS_ARGUMENT_TYPE_ENUM, 8U, 0},
    {"srid", VPS_ARGUMENT_TYPE_UINT32, 10U, 0},
    {"connect_timeout", VPS_ARGUMENT_TYPE_UINT32, 10U, 0},
    {"statement_timeout", VPS_ARGUMENT_TYPE_UINT32, 10U, 0},
    {"lock_timeout", VPS_ARGUMENT_TYPE_UINT32, 10U, 0},
    {"pool_min", VPS_ARGUMENT_TYPE_UINT32, 10U, 0},
    {"pool_max", VPS_ARGUMENT_TYPE_UINT32, 10U, 0},
    {"pool_idle_timeout", VPS_ARGUMENT_TYPE_UINT32, 10U, 0},
    {"pool_wait_timeout", VPS_ARGUMENT_TYPE_UINT32, 10U, 0},
    {"pool_validation_interval", VPS_ARGUMENT_TYPE_UINT32, 10U, 0},
    {"pool_reset", VPS_ARGUMENT_TYPE_ENUM, 16U, 0},
    {"pool_readonly_separate", VPS_ARGUMENT_TYPE_BOOLEAN, 5U, 0},
    {"metadata_mode", VPS_ARGUMENT_TYPE_ENUM, 8U, 0}};

_Static_assert(sizeof(vps_argument_definitions) /
                       sizeof(vps_argument_definitions[0]) ==
                   VPS_ARGUMENT_ID_COUNT,
               "argument definition table must match VpsArgumentId");

static int vps_argument_id_is_valid(VpsArgumentId argument_id)
{
    return argument_id >= VPS_ARGUMENT_ID_CREDENTIAL_REF &&
           argument_id < VPS_ARGUMENT_ID_COUNT;
}

static char vps_argument_ascii_lower(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

static int vps_argument_name_character_is_valid(char value, size_t index)
{
    value = vps_argument_ascii_lower(value);
    if (value >= 'a' && value <= 'z') {
        return 1;
    }
    return index != 0U &&
           ((value >= '0' && value <= '9') || value == '_');
}

static int vps_argument_utf8_is_valid(const unsigned char *value,
                                      size_t length)
{
    size_t index = 0U;

    while (index < length) {
        unsigned char first = value[index];
        uint32_t code_point;
        size_t continuation_count;
        size_t continuation_index;

        if (first <= 0x7fU) {
            index += 1U;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            code_point = first & 0x1fU;
            continuation_count = 1U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            code_point = first & 0x0fU;
            continuation_count = 2U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            code_point = first & 0x07U;
            continuation_count = 3U;
        } else {
            return 0;
        }
        if (continuation_count > length - index - 1U) {
            return 0;
        }
        for (continuation_index = 1U;
             continuation_index <= continuation_count;
             ++continuation_index) {
            unsigned char continuation = value[index + continuation_index];
            if ((continuation & 0xc0U) != 0x80U) {
                return 0;
            }
            code_point = (code_point << 6) | (continuation & 0x3fU);
        }
        if ((continuation_count == 2U && code_point < 0x800U) ||
            (continuation_count == 3U && code_point < 0x10000U) ||
            code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return 0;
        }
        index += continuation_count + 1U;
    }
    return 1;
}

static int vps_argument_value_bytes_are_valid(const char *value,
                                              size_t length)
{
    size_t index;

    if (length == 0U || !vps_argument_utf8_is_valid(
                            (const unsigned char *)value, length)) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        unsigned char current = (unsigned char)value[index];
        if (current == 0U || current < 0x20U || current == 0x7fU) {
            return 0;
        }
    }
    return 1;
}

static VpsArgumentId vps_argument_find_id(const char *name, size_t length)
{
    size_t index;

    for (index = 0U; index < VPS_ARGUMENT_ID_COUNT; ++index) {
        const char *candidate = vps_argument_definitions[index].name;
        if (strlen(candidate) == length &&
            memcmp(candidate, name, length) == 0) {
            return (VpsArgumentId)index;
        }
    }
    return VPS_ARGUMENT_ID_UNKNOWN;
}

static int vps_argument_decode_value(const char *input,
                                     size_t length,
                                     char *output,
                                     size_t *output_length)
{
    size_t input_index;
    size_t output_index = 0U;
    int quoted;

    if (input == NULL || output == NULL || output_length == NULL ||
        length == 0U) {
        return 0;
    }
    quoted = input[0] == '\'';
    if (quoted) {
        if (length < 2U || input[length - 1U] != '\'') {
            return 0;
        }
        input_index = 1U;
        while (input_index < length - 1U) {
            if (input[input_index] == '\'') {
                if (input_index + 1U >= length - 1U ||
                    input[input_index + 1U] != '\'') {
                    return 0;
                }
                input_index += 1U;
            }
            output[output_index++] = input[input_index++];
        }
    } else {
        for (input_index = 0U; input_index < length; ++input_index) {
            if (input[input_index] == '\'') {
                return 0;
            }
            output[output_index++] = input[input_index];
        }
    }
    if (!vps_argument_value_bytes_are_valid(output, output_index)) {
        return 0;
    }
    output[output_index] = '\0';
    *output_length = output_index;
    return 1;
}

static int vps_argument_parse_uint32(const char *value,
                                     size_t length,
                                     uint32_t *parsed)
{
    uint32_t result = 0U;
    size_t index;

    if (length == 0U || parsed == NULL) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        uint32_t digit;
        if (value[index] < '0' || value[index] > '9') {
            return 0;
        }
        digit = (uint32_t)(value[index] - '0');
        if (result > (UINT32_MAX - digit) / 10U) {
            return 0;
        }
        result = result * 10U + digit;
    }
    *parsed = result;
    return 1;
}

static int vps_argument_parse_boolean(const char *value,
                                      size_t length,
                                      int *parsed)
{
    if (length == 4U && memcmp(value, "true", 4U) == 0) {
        *parsed = 1;
        return 1;
    }
    if (length == 5U && memcmp(value, "false", 5U) == 0) {
        *parsed = 0;
        return 1;
    }
    return 0;
}

static int vps_argument_parse_enum(VpsArgumentId argument_id,
                                   const char *value,
                                   size_t length,
                                   VpsArgumentEnumValue *parsed)
{
    if (argument_id == VPS_ARGUMENT_ID_SOURCE) {
        if (length == 5U && memcmp(value, "table", 5U) == 0) {
            *parsed = VPS_ARGUMENT_ENUM_SOURCE_TABLE;
            return 1;
        }
        if (length == 5U && memcmp(value, "query", 5U) == 0) {
            *parsed = VPS_ARGUMENT_ENUM_SOURCE_QUERY;
            return 1;
        }
    } else if (argument_id == VPS_ARGUMENT_ID_MODE) {
        if (length == 2U && memcmp(value, "ro", 2U) == 0) {
            *parsed = VPS_ARGUMENT_ENUM_MODE_RO;
            return 1;
        }
        if (length == 2U && memcmp(value, "rw", 2U) == 0) {
            *parsed = VPS_ARGUMENT_ENUM_MODE_RW;
            return 1;
        }
    } else if (argument_id == VPS_ARGUMENT_ID_GEOMETRY) {
        static const char *const names[] = {"wkt", "wkb", "ewkt", "ewkb"};
        size_t index;
        for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
            if (strlen(names[index]) == length &&
                memcmp(value, names[index], length) == 0) {
                *parsed = (VpsArgumentEnumValue)(
                    VPS_ARGUMENT_ENUM_GEOMETRY_WKT + index);
                return 1;
            }
        }
    } else if (argument_id == VPS_ARGUMENT_ID_POOL_RESET) {
        if (length == 11U && memcmp(value, "discard_all", 11U) == 0) {
            *parsed = VPS_ARGUMENT_ENUM_POOL_DISCARD_ALL;
            return 1;
        }
        if (length == 12U && memcmp(value, "strict_reset", 12U) == 0) {
            *parsed = VPS_ARGUMENT_ENUM_POOL_STRICT_RESET;
            return 1;
        }
    } else if (argument_id == VPS_ARGUMENT_ID_METADATA_MODE) {
        if (length == 4U && memcmp(value, "live", 4U) == 0) {
            *parsed = VPS_ARGUMENT_ENUM_METADATA_LIVE;
            return 1;
        }
        if (length == 6U && memcmp(value, "cached", 6U) == 0) {
            *parsed = VPS_ARGUMENT_ENUM_METADATA_CACHED;
            return 1;
        }
    }
    return 0;
}

static void vps_arguments_set_diagnostic(VpsArgumentsDiagnostic *diagnostic,
                                         VpsArgumentsResult result,
                                         VpsArgumentId argument_id,
                                         VpsArgumentMask presence)
{
    if (diagnostic == NULL) {
        return;
    }
    diagnostic->result = result;
    diagnostic->argument_id = argument_id;
    diagnostic->presence = presence;
    diagnostic->sensitive = vps_argument_is_sensitive(argument_id);
}

static void vps_arguments_log(VpsLogger *logger,
                              const VpsArgumentsDiagnostic *diagnostic)
{
    static const char operation[] = "argument_parser";
    static const char phase[] = "validation";
    const char *argument_name;
    const char *status;
    VpsLogEvent event;

    if (logger == NULL || diagnostic == NULL) {
        return;
    }
    argument_name = vps_argument_name(diagnostic->argument_id);
    status = vps_arguments_result_name(diagnostic->result);
    if (vps_log_event_init(&event,
                           diagnostic->result == VPS_ARGUMENTS_OK
                               ? VPS_LOG_LEVEL_DEBUG
                               : VPS_LOG_LEVEL_WARN) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase,
                                 sizeof(phase) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_ARGUMENT,
                                 argument_name,
                                 strlen(argument_name)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PRESENCE_MASK,
                                 diagnostic->presence) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK) {
        return;
    }
    vps_logger_emit(logger, &event);
}

static VpsArgumentsResult vps_arguments_parse_one(
    VpsParsedArguments *parsed,
    const VpsArgumentInput *input,
    size_t *storage_offset,
    VpsArgumentsDiagnostic *diagnostic)
{
    char normalized_name[VPS_ARGUMENT_NAME_LIMIT + 1U];
    char *storage = (char *)vps_sensitive_memory_data(&parsed->storage);
    const VpsArgumentDefinition *definition;
    VpsArgumentValue value;
    VpsArgumentId argument_id;
    size_t begin = 0U;
    size_t end = input->length;
    size_t equals;
    size_t name_begin;
    size_t name_end;
    size_t value_begin;
    size_t value_end;
    size_t name_length;
    size_t decoded_length;
    size_t index;

    while (begin < end && input->text[begin] == ' ') {
        begin += 1U;
    }
    while (end > begin && input->text[end - 1U] == ' ') {
        end -= 1U;
    }
    for (equals = begin; equals < end && input->text[equals] != '=';
         ++equals) {
    }
    if (equals == begin || equals == end) {
        vps_arguments_set_diagnostic(diagnostic, VPS_ARGUMENTS_MALFORMED,
                                     VPS_ARGUMENT_ID_UNKNOWN,
                                     parsed->presence);
        return VPS_ARGUMENTS_MALFORMED;
    }
    name_begin = begin;
    name_end = equals;
    while (name_end > name_begin && input->text[name_end - 1U] == ' ') {
        name_end -= 1U;
    }
    value_begin = equals + 1U;
    value_end = end;
    while (value_begin < value_end && input->text[value_begin] == ' ') {
        value_begin += 1U;
    }
    while (value_end > value_begin && input->text[value_end - 1U] == ' ') {
        value_end -= 1U;
    }
    name_length = name_end - name_begin;
    if (name_length == 0U || name_length > VPS_ARGUMENT_NAME_LIMIT ||
        value_begin == value_end) {
        vps_arguments_set_diagnostic(diagnostic, VPS_ARGUMENTS_MALFORMED,
                                     VPS_ARGUMENT_ID_UNKNOWN,
                                     parsed->presence);
        return VPS_ARGUMENTS_MALFORMED;
    }
    for (index = 0U; index < name_length; ++index) {
        char current = input->text[name_begin + index];
        if (!vps_argument_name_character_is_valid(current, index)) {
            vps_arguments_set_diagnostic(diagnostic, VPS_ARGUMENTS_MALFORMED,
                                         VPS_ARGUMENT_ID_UNKNOWN,
                                         parsed->presence);
            return VPS_ARGUMENTS_MALFORMED;
        }
        normalized_name[index] = vps_argument_ascii_lower(current);
    }
    normalized_name[name_length] = '\0';
    argument_id = vps_argument_find_id(normalized_name, name_length);
    if (!vps_argument_id_is_valid(argument_id)) {
        vps_arguments_set_diagnostic(diagnostic,
                                     VPS_ARGUMENTS_UNKNOWN_ARGUMENT,
                                     VPS_ARGUMENT_ID_UNKNOWN,
                                     parsed->presence);
        return VPS_ARGUMENTS_UNKNOWN_ARGUMENT;
    }
    if ((parsed->presence & (UINT64_C(1) << argument_id)) != 0U) {
        vps_arguments_set_diagnostic(diagnostic,
                                     VPS_ARGUMENTS_DUPLICATE_ARGUMENT,
                                     argument_id, parsed->presence);
        return VPS_ARGUMENTS_DUPLICATE_ARGUMENT;
    }
    definition = &vps_argument_definitions[(size_t)argument_id];
    if (!vps_argument_decode_value(input->text + value_begin,
                                   value_end - value_begin,
                                   storage + *storage_offset,
                                   &decoded_length)) {
        vps_arguments_set_diagnostic(diagnostic, VPS_ARGUMENTS_MALFORMED,
                                     argument_id, parsed->presence);
        return VPS_ARGUMENTS_MALFORMED;
    }
    if (decoded_length > definition->limit) {
        vps_arguments_set_diagnostic(diagnostic,
                                     VPS_ARGUMENTS_LIMIT_EXCEEDED,
                                     argument_id, parsed->presence);
        return VPS_ARGUMENTS_LIMIT_EXCEEDED;
    }

    (void)memset(&value, 0, sizeof(value));
    value.type = definition->type;
    value.offset = *storage_offset;
    value.length = decoded_length;
    value.present = 1;
    if (definition->type == VPS_ARGUMENT_TYPE_UINT32 &&
        !vps_argument_parse_uint32(storage + value.offset, value.length,
                                   &value.uint32_value)) {
        vps_arguments_set_diagnostic(diagnostic, VPS_ARGUMENTS_RANGE_ERROR,
                                     argument_id, parsed->presence);
        return VPS_ARGUMENTS_RANGE_ERROR;
    }
    if (definition->type == VPS_ARGUMENT_TYPE_BOOLEAN &&
        !vps_argument_parse_boolean(storage + value.offset, value.length,
                                    &value.boolean_value)) {
        vps_arguments_set_diagnostic(diagnostic, VPS_ARGUMENTS_MALFORMED,
                                     argument_id, parsed->presence);
        return VPS_ARGUMENTS_MALFORMED;
    }
    if (definition->type == VPS_ARGUMENT_TYPE_ENUM &&
        !vps_argument_parse_enum(argument_id, storage + value.offset,
                                 value.length, &value.enum_value)) {
        vps_arguments_set_diagnostic(diagnostic, VPS_ARGUMENTS_MALFORMED,
                                     argument_id, parsed->presence);
        return VPS_ARGUMENTS_MALFORMED;
    }
    parsed->values[(size_t)argument_id] = value;
    parsed->presence |= UINT64_C(1) << argument_id;
    *storage_offset += decoded_length + 1U;
    vps_arguments_set_diagnostic(diagnostic, VPS_ARGUMENTS_OK, argument_id,
                                 parsed->presence);
    return VPS_ARGUMENTS_OK;
}

static VpsArgumentsResult vps_arguments_validate_combinations(
    const VpsParsedArguments *arguments,
    VpsArgumentsDiagnostic *diagnostic)
{
    VpsArgumentMask modes = arguments->presence &
                            VPS_ARGUMENT_CONNECTION_MODES;
    const VpsArgumentValue *source =
        &arguments->values[VPS_ARGUMENT_ID_SOURCE];
    const VpsArgumentValue *mode = &arguments->values[VPS_ARGUMENT_ID_MODE];

    if (modes == 0U || (modes & (modes - 1U)) != 0U) {
        vps_arguments_set_diagnostic(diagnostic,
                                     VPS_ARGUMENTS_INCOMPATIBLE,
                                     VPS_ARGUMENT_ID_UNKNOWN,
                                     arguments->presence);
        return VPS_ARGUMENTS_INCOMPATIBLE;
    }
    if (!source->present) {
        vps_arguments_set_diagnostic(diagnostic,
                                     VPS_ARGUMENTS_INCOMPATIBLE,
                                     VPS_ARGUMENT_ID_SOURCE,
                                     arguments->presence);
        return VPS_ARGUMENTS_INCOMPATIBLE;
    }
    if ((arguments->presence & VPS_ARGUMENT_SERVICE_FILE) != 0U &&
        (arguments->presence & VPS_ARGUMENT_SERVICE) == 0U) {
        vps_arguments_set_diagnostic(diagnostic,
                                     VPS_ARGUMENTS_INCOMPATIBLE,
                                     VPS_ARGUMENT_ID_SERVICE_FILE,
                                     arguments->presence);
        return VPS_ARGUMENTS_INCOMPATIBLE;
    }
    if (source->enum_value == VPS_ARGUMENT_ENUM_SOURCE_TABLE) {
        if ((arguments->presence & (VPS_ARGUMENT_SCHEMA | VPS_ARGUMENT_TABLE)) !=
                (VPS_ARGUMENT_SCHEMA | VPS_ARGUMENT_TABLE) ||
            (arguments->presence &
             (VPS_ARGUMENT_QUERY | VPS_ARGUMENT_QUERY_PROFILE)) != 0U) {
            vps_arguments_set_diagnostic(diagnostic,
                                         VPS_ARGUMENTS_INCOMPATIBLE,
                                         VPS_ARGUMENT_ID_SOURCE,
                                         arguments->presence);
            return VPS_ARGUMENTS_INCOMPATIBLE;
        }
    } else {
        VpsArgumentMask query_sources =
            arguments->presence &
            (VPS_ARGUMENT_QUERY | VPS_ARGUMENT_QUERY_PROFILE);
        if (query_sources == 0U ||
            (query_sources & (query_sources - 1U)) != 0U ||
            (arguments->presence &
             (VPS_ARGUMENT_SCHEMA | VPS_ARGUMENT_TABLE |
              VPS_ARGUMENT_ALLOW_VIEW | VPS_ARGUMENT_ALLOW_MATERIALIZED_VIEW |
              VPS_ARGUMENT_ALLOW_FOREIGN_TABLE)) != 0U ||
            (mode->present && mode->enum_value == VPS_ARGUMENT_ENUM_MODE_RW)) {
            vps_arguments_set_diagnostic(diagnostic,
                                         VPS_ARGUMENTS_INCOMPATIBLE,
                                         VPS_ARGUMENT_ID_SOURCE,
                                         arguments->presence);
            return VPS_ARGUMENTS_INCOMPATIBLE;
        }
    }
    if (arguments->values[VPS_ARGUMENT_ID_POOL_MIN].present &&
        arguments->values[VPS_ARGUMENT_ID_POOL_MAX].present &&
        arguments->values[VPS_ARGUMENT_ID_POOL_MIN].uint32_value >
            arguments->values[VPS_ARGUMENT_ID_POOL_MAX].uint32_value) {
        vps_arguments_set_diagnostic(diagnostic,
                                     VPS_ARGUMENTS_INCOMPATIBLE,
                                     VPS_ARGUMENT_ID_POOL_MIN,
                                     arguments->presence);
        return VPS_ARGUMENTS_INCOMPATIBLE;
    }
    return VPS_ARGUMENTS_OK;
}

VpsArgumentsResult vps_arguments_init(
    VpsParsedArguments *arguments,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger)
{
    if (arguments == NULL ||
        vps_sensitive_memory_init(&arguments->storage, allocator, operations,
                                  logger) != VPS_SECURE_MEMORY_OK) {
        return VPS_ARGUMENTS_INVALID_ARGUMENT;
    }
    (void)memset(arguments->values, 0, sizeof(arguments->values));
    arguments->presence = 0U;
    arguments->initialized = 1;
    return VPS_ARGUMENTS_OK;
}

VpsArgumentsResult vps_arguments_parse(
    VpsParsedArguments *arguments,
    const VpsArgumentInput *inputs,
    size_t input_count,
    VpsArgumentsDiagnostic *diagnostic)
{
    VpsParsedArguments replacement;
    VpsArgumentsDiagnostic local_diagnostic;
    VpsArgumentsResult result = VPS_ARGUMENTS_OK;
    VpsSecureMemoryResult secure_result;
    size_t total_size = 0U;
    size_t storage_offset = 0U;
    size_t index;

    vps_arguments_set_diagnostic(&local_diagnostic, VPS_ARGUMENTS_OK,
                                 VPS_ARGUMENT_ID_UNKNOWN, 0U);
    if (arguments == NULL || !arguments->initialized || inputs == NULL ||
        input_count == 0U || input_count > VPS_ARGUMENT_MAX_COUNT) {
        vps_arguments_set_diagnostic(&local_diagnostic,
                                     VPS_ARGUMENTS_INVALID_ARGUMENT,
                                     VPS_ARGUMENT_ID_UNKNOWN, 0U);
        if (diagnostic != NULL) {
            *diagnostic = local_diagnostic;
        }
        return VPS_ARGUMENTS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < input_count; ++index) {
        size_t required;
        if (inputs[index].text == NULL || inputs[index].length == 0U ||
            inputs[index].length > VPS_ARGUMENT_VALUE_LIMIT ||
            vps_size_add(inputs[index].length, 1U, &required) !=
                VPS_MEMORY_OK ||
            vps_size_add(total_size, required, &total_size) != VPS_MEMORY_OK ||
            total_size > VPS_ARGUMENT_TOTAL_LIMIT) {
            vps_arguments_set_diagnostic(&local_diagnostic,
                                         VPS_ARGUMENTS_LIMIT_EXCEEDED,
                                         VPS_ARGUMENT_ID_UNKNOWN, 0U);
            result = VPS_ARGUMENTS_LIMIT_EXCEEDED;
            goto done;
        }
    }
    if (vps_arguments_init(&replacement,
                           &arguments->storage.storage.allocator,
                           arguments->storage.operations,
                           arguments->storage.logger) != VPS_ARGUMENTS_OK) {
        result = VPS_ARGUMENTS_INVALID_ARGUMENT;
        goto done;
    }
    secure_result = vps_sensitive_memory_allocate(&replacement.storage,
                                                  total_size);
    if (secure_result != VPS_SECURE_MEMORY_OK) {
        result = secure_result == VPS_SECURE_MEMORY_OUT_OF_MEMORY
                     ? VPS_ARGUMENTS_OUT_OF_MEMORY
                     : VPS_ARGUMENTS_INVALID_ARGUMENT;
        vps_arguments_set_diagnostic(&local_diagnostic, result,
                                     VPS_ARGUMENT_ID_UNKNOWN, 0U);
        goto replacement_done;
    }
    (void)memset(vps_sensitive_memory_data(&replacement.storage), 0,
                 total_size);
    for (index = 0U; index < input_count; ++index) {
        result = vps_arguments_parse_one(&replacement, &inputs[index],
                                         &storage_offset,
                                         &local_diagnostic);
        if (result != VPS_ARGUMENTS_OK) {
            goto replacement_done;
        }
        vps_arguments_log(replacement.storage.logger, &local_diagnostic);
    }
    result = vps_arguments_validate_combinations(&replacement,
                                                 &local_diagnostic);
    if (result != VPS_ARGUMENTS_OK) {
        goto replacement_done;
    }
    secure_result = vps_sensitive_memory_release(&arguments->storage);
    if (secure_result != VPS_SECURE_MEMORY_OK) {
        result = VPS_ARGUMENTS_CLEANUP_FAILED;
        vps_arguments_set_diagnostic(&local_diagnostic, result,
                                     VPS_ARGUMENT_ID_UNKNOWN,
                                     arguments->presence);
        goto replacement_done;
    }
    *arguments = replacement;
    vps_arguments_set_diagnostic(&local_diagnostic, VPS_ARGUMENTS_OK,
                                 VPS_ARGUMENT_ID_UNKNOWN,
                                 arguments->presence);
    goto done;

replacement_done:
    if (vps_sensitive_memory_release(&replacement.storage) !=
        VPS_SECURE_MEMORY_OK) {
        result = VPS_ARGUMENTS_CLEANUP_FAILED;
        vps_arguments_set_diagnostic(&local_diagnostic, result,
                                     VPS_ARGUMENT_ID_UNKNOWN,
                                     arguments->presence);
    }

done:
    if (diagnostic != NULL) {
        *diagnostic = local_diagnostic;
    }
    vps_arguments_log(arguments->storage.logger, &local_diagnostic);
    return result;
}

VpsArgumentsResult vps_arguments_reset(VpsParsedArguments *arguments)
{
    if (arguments == NULL || !arguments->initialized) {
        return VPS_ARGUMENTS_INVALID_ARGUMENT;
    }
    if (vps_sensitive_memory_release(&arguments->storage) !=
        VPS_SECURE_MEMORY_OK) {
        return VPS_ARGUMENTS_CLEANUP_FAILED;
    }
    (void)memset(arguments->values, 0, sizeof(arguments->values));
    arguments->presence = 0U;
    return VPS_ARGUMENTS_OK;
}

const VpsArgumentValue *vps_arguments_get(const VpsParsedArguments *arguments,
                                          VpsArgumentId argument_id)
{
    if (arguments == NULL || !arguments->initialized ||
        !vps_argument_id_is_valid(argument_id) ||
        !arguments->values[(size_t)argument_id].present) {
        return NULL;
    }
    return &arguments->values[(size_t)argument_id];
}

const char *vps_argument_text(const VpsParsedArguments *arguments,
                              VpsArgumentId argument_id,
                              size_t *length)
{
    const VpsArgumentValue *value = vps_arguments_get(arguments, argument_id);
    const char *storage;

    if (value == NULL || length == NULL) {
        return NULL;
    }
    storage = (const char *)arguments->storage.storage.memory;
    if (storage == NULL || value->offset > arguments->storage.storage.size ||
        value->length >= arguments->storage.storage.size - value->offset) {
        return NULL;
    }
    *length = value->length;
    return storage + value->offset;
}

const char *vps_argument_name(VpsArgumentId argument_id)
{
    if (!vps_argument_id_is_valid(argument_id)) {
        return "unknown";
    }
    return vps_argument_definitions[(size_t)argument_id].name;
}

int vps_argument_is_sensitive(VpsArgumentId argument_id)
{
    return vps_argument_id_is_valid(argument_id) &&
           vps_argument_definitions[(size_t)argument_id].sensitive;
}

const char *vps_arguments_result_name(VpsArgumentsResult result)
{
    static const char *const names[] = {
        "ok",          "invalid_argument", "unknown_argument",
        "duplicate_argument", "malformed", "limit_exceeded",
        "range_error", "incompatible",     "out_of_memory",
        "cleanup_failed"};

    if (result < VPS_ARGUMENTS_OK ||
        result > VPS_ARGUMENTS_CLEANUP_FAILED) {
        return "unknown";
    }
    return names[(size_t)result];
}
