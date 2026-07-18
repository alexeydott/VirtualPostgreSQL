#include "vps_type_registry.h"

#include <string.h>

static int vps_type_name_equals(const VpsColumnSet *columns,
                                const VpsMetadataString *string,
                                const char *expected)
{
    const unsigned char *value;
    size_t length;
    size_t expected_length = strlen(expected);
    return vps_column_set_string(columns, string, &value, &length) ==
               VPS_METADATA_OK &&
           length == expected_length && memcmp(value, expected, length) == 0;
}

static int vps_type_is_pg_catalog(const VpsColumnSet *columns,
                                  const VpsMetadataString *namespace_name)
{
    return vps_type_name_equals(columns, namespace_name, "pg_catalog");
}

static int vps_type_is_any(const VpsColumnSet *columns,
                           const VpsMetadataString *name,
                           const char *const *values,
                           size_t value_count)
{
    size_t index;
    for (index = 0U; index < value_count; ++index) {
        if (vps_type_name_equals(columns, name, values[index])) return 1;
    }
    return 0;
}

static void vps_type_log(const VpsTypeRegistry *registry,
                         const VpsTypeSelection *selection)
{
    VpsLogEvent event;
    static const char operation[] = "type_registry_select";
    static const char status[] = "passed";
    const char *codec;
    if (registry == NULL || registry->logger == NULL || selection == NULL)
        return;
    codec = vps_codec_name(selection->codec);
    if (vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 sizeof(status) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_EXPECTED_CLASS, codec,
                                 strlen(codec)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_FORMAT_VERSION,
                                 registry->version) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_PRESENCE_MASK,
                                 selection->capabilities) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_TYPE_OID,
                                 selection->effective_type_oid) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_CODEC_ID,
                                 (uint64_t)selection->codec) != VPS_LOG_OK)
        return;
    vps_logger_emit(registry->logger, &event);
}

VpsMetadataResult vps_type_registry_init(VpsTypeRegistry *registry,
                                         VpsLogger *logger)
{
    if (registry == NULL) return VPS_METADATA_INVALID_ARGUMENT;
    registry->version = VPS_TYPE_REGISTRY_VERSION;
    registry->logger = logger;
    registry->initialized = 1;
    return VPS_METADATA_OK;
}

VpsMetadataResult vps_type_registry_select(
    const VpsTypeRegistry *registry,
    const VpsColumnSet *columns,
    size_t column_index,
    VpsTypeSelection *selection)
{
    static const char *const integer_names[] = {
        "int2", "int4", "int8", "oid", "xid", "cid"};
    static const char *const float_names[] = {"float4", "float8"};
    static const char *const text_names[] = {
        "text", "varchar", "bpchar", "name", "xml", "inet", "cidr",
        "macaddr", "macaddr8", "bit", "varbit"};
    static const char *const datetime_names[] = {
        "date", "time", "timetz", "timestamp", "timestamptz", "interval"};
    static const char *const json_names[] = {"json", "jsonb"};
    const VpsColumnMetadata *column;
    const VpsMetadataString *namespace_name;
    const VpsMetadataString *type_name;
    char type_kind;
    int pg_catalog;
    if (registry == NULL || !registry->initialized ||
        registry->version != VPS_TYPE_REGISTRY_VERSION || columns == NULL ||
        !columns->initialized || selection == NULL ||
        column_index >= columns->column_count)
        return VPS_METADATA_INVALID_ARGUMENT;
    column = &columns->columns[column_index];
    (void)memset(selection, 0, sizeof(*selection));
    selection->declared_type_oid = column->type_oid;
    selection->domain = column->type_kind == 'd';
    selection->effective_type_oid = selection->domain
                                        ? column->domain_base_oid
                                        : column->type_oid;
    namespace_name = selection->domain ? &column->domain_base_namespace
                                       : &column->type_namespace;
    type_name = selection->domain ? &column->domain_base_name
                                  : &column->type_name;
    type_kind = selection->domain ? column->domain_base_kind
                                  : column->type_kind;
    if (!namespace_name->present || !type_name->present ||
        selection->effective_type_oid == 0U) return VPS_METADATA_INVALID_RESULT;
    pg_catalog = vps_type_is_pg_catalog(columns, namespace_name);
    selection->codec = VPS_CODEC_USER_TEXT;
    selection->capabilities = VPS_CODEC_CAP_READ;
    if (type_kind == 'e') {
        selection->codec = VPS_CODEC_ENUM_TEXT;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_EQUALITY |
                                  VPS_CODEC_CAP_DML;
    } else if (type_kind == 'r' || type_kind == 'm') {
        selection->codec = VPS_CODEC_RANGE_TEXT;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_DML;
    } else if (type_kind == 'c') {
        selection->codec = VPS_CODEC_COMPOSITE_TEXT;
    } else if (column->type_category == 'A' && !selection->domain) {
        selection->codec = VPS_CODEC_ARRAY_TEXT;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_DML;
    } else if (pg_catalog &&
               vps_type_name_equals(columns, type_name, "bool")) {
        selection->codec = VPS_CODEC_BOOLEAN;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_EQUALITY |
                                  VPS_CODEC_CAP_ORDER | VPS_CODEC_CAP_DML |
                                  VPS_CODEC_CAP_PUSHDOWN_EXACT;
    } else if (pg_catalog &&
               vps_type_is_any(columns, type_name, integer_names,
                               sizeof(integer_names) / sizeof(integer_names[0]))) {
        selection->codec = VPS_CODEC_INTEGER;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_EQUALITY |
                                  VPS_CODEC_CAP_ORDER | VPS_CODEC_CAP_DML |
                                  VPS_CODEC_CAP_PUSHDOWN_EXACT;
    } else if (pg_catalog &&
               vps_type_is_any(columns, type_name, float_names,
                               sizeof(float_names) / sizeof(float_names[0]))) {
        selection->codec = VPS_CODEC_FLOAT;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_DML;
    } else if (pg_catalog &&
               vps_type_name_equals(columns, type_name, "numeric")) {
        selection->codec = VPS_CODEC_NUMERIC_TEXT;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_DML;
    } else if (pg_catalog &&
               vps_type_name_equals(columns, type_name, "money")) {
        selection->codec = VPS_CODEC_MONEY_TEXT;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_DML;
    } else if (pg_catalog &&
               vps_type_name_equals(columns, type_name, "bytea")) {
        selection->codec = VPS_CODEC_BYTEA;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_EQUALITY |
                                  VPS_CODEC_CAP_DML |
                                  VPS_CODEC_CAP_PUSHDOWN_EXACT |
                                  VPS_CODEC_CAP_BINARY;
    } else if (pg_catalog &&
               vps_type_name_equals(columns, type_name, "uuid")) {
        selection->codec = VPS_CODEC_UUID_TEXT;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_EQUALITY |
                                  VPS_CODEC_CAP_ORDER | VPS_CODEC_CAP_DML |
                                  VPS_CODEC_CAP_PUSHDOWN_EXACT;
    } else if (pg_catalog &&
               vps_type_is_any(columns, type_name, datetime_names,
                               sizeof(datetime_names) /
                                   sizeof(datetime_names[0]))) {
        selection->codec = VPS_CODEC_DATETIME_TEXT;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_EQUALITY |
                                  VPS_CODEC_CAP_ORDER | VPS_CODEC_CAP_DML;
    } else if (pg_catalog &&
               vps_type_is_any(columns, type_name, json_names,
                               sizeof(json_names) / sizeof(json_names[0]))) {
        selection->codec = VPS_CODEC_JSON_TEXT;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_DML;
    } else if (pg_catalog &&
               vps_type_is_any(columns, type_name, text_names,
                               sizeof(text_names) / sizeof(text_names[0]))) {
        selection->codec = VPS_CODEC_TEXT;
        selection->capabilities = VPS_CODEC_CAP_READ | VPS_CODEC_CAP_DML;
    }
    vps_type_log(registry, selection);
    return VPS_METADATA_OK;
}

const char *vps_codec_name(VpsCodecId codec)
{
    switch (codec) {
        case VPS_CODEC_BOOLEAN: return "boolean";
        case VPS_CODEC_INTEGER: return "integer";
        case VPS_CODEC_FLOAT: return "float";
        case VPS_CODEC_NUMERIC_TEXT: return "numeric_text";
        case VPS_CODEC_MONEY_TEXT: return "money_text";
        case VPS_CODEC_TEXT: return "text";
        case VPS_CODEC_BYTEA: return "bytea";
        case VPS_CODEC_DATETIME_TEXT: return "datetime_text";
        case VPS_CODEC_UUID_TEXT: return "uuid_text";
        case VPS_CODEC_JSON_TEXT: return "json_text";
        case VPS_CODEC_ARRAY_TEXT: return "array_text";
        case VPS_CODEC_ENUM_TEXT: return "enum_text";
        case VPS_CODEC_RANGE_TEXT: return "range_text";
        case VPS_CODEC_COMPOSITE_TEXT: return "composite_text";
        case VPS_CODEC_USER_TEXT: return "user_text";
        default: return "unknown";
    }
}
