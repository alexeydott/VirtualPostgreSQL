#include "vps_schema_fingerprint.h"

#include <string.h>

#define VPS_FNV_PRIME UINT64_C(1099511628211)

static void vps_hash_bytes(uint64_t *hash,
                           const unsigned char *value,
                           size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        *hash ^= (uint64_t)value[index];
        *hash *= VPS_FNV_PRIME;
    }
}

static void vps_hash_u32(uint64_t *hash, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24) & UINT32_C(0xff));
    vps_hash_bytes(hash, bytes, sizeof(bytes));
}

static void vps_hash_u64(uint64_t *hash, uint64_t value)
{
    unsigned char bytes[8];
    size_t index;
    for (index = 0U; index < sizeof(bytes); ++index)
        bytes[index] = (unsigned char)((value >> (index * 8U)) & UINT64_C(0xff));
    vps_hash_bytes(hash, bytes, sizeof(bytes));
}

static int vps_hash_string(uint64_t *hash,
                           const VpsColumnSet *columns,
                           const VpsMetadataString *string)
{
    const unsigned char *value;
    size_t length;
    vps_hash_u32(hash, string->present ? 1U : 0U);
    if (!string->present) return 1;
    if (vps_column_set_string(columns, string, &value, &length) !=
            VPS_METADATA_OK ||
        length > UINT32_MAX) return 0;
    vps_hash_u32(hash, (uint32_t)length);
    vps_hash_bytes(hash, value, length);
    return 1;
}

static uint64_t vps_relation_component(const VpsRelationMetadata *relation)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    vps_hash_u32(&hash, relation->namespace_oid);
    vps_hash_u32(&hash, relation->relation_oid);
    vps_hash_u32(&hash, relation->access_method_oid);
    vps_hash_u32(&hash, (uint32_t)relation->kind);
    vps_hash_u32(&hash, (uint32_t)(unsigned char)relation->persistence);
    vps_hash_u32(&hash, relation->is_partition ? 1U : 0U);
    vps_hash_u32(&hash, relation->has_children ? 1U : 0U);
    vps_hash_u32(&hash, relation->has_parent ? 1U : 0U);
    return hash;
}

static int vps_layout_component(const VpsColumnSet *columns,
                                uint64_t *component)
{
    uint64_t hash = UINT64_C(1099511628211);
    size_t index;
    vps_hash_u64(&hash, (uint64_t)columns->column_count);
    for (index = 0U; index < columns->column_count; ++index) {
        const VpsColumnMetadata *column = &columns->columns[index];
        vps_hash_u32(&hash, (uint32_t)column->attribute_number);
        vps_hash_u32(&hash, column->type_oid);
        vps_hash_u32(&hash, (uint32_t)column->type_modifier);
        vps_hash_u32(&hash, column->type_namespace_oid);
        vps_hash_u32(&hash, column->domain_base_oid);
        vps_hash_u32(&hash, (uint32_t)column->domain_base_modifier);
        vps_hash_u32(&hash, column->array_element_oid);
        vps_hash_u32(&hash, column->collation_oid);
        vps_hash_u32(&hash, (uint32_t)(unsigned char)column->type_category);
        vps_hash_u32(&hash, (uint32_t)(unsigned char)column->type_kind);
        vps_hash_u32(&hash, (uint32_t)(unsigned char)column->generated_kind);
        vps_hash_u32(&hash, (uint32_t)(unsigned char)column->identity_kind);
        vps_hash_u32(&hash, (uint32_t)(unsigned char)column->default_kind);
        vps_hash_u32(&hash, column->not_null ? 1U : 0U);
        vps_hash_u32(&hash, column->dropped ? 1U : 0U);
        vps_hash_u32(&hash, column->has_default ? 1U : 0U);
        vps_hash_u32(&hash, column->domain_not_null ? 1U : 0U);
        vps_hash_u32(&hash, column->domain_has_default ? 1U : 0U);
        vps_hash_u32(&hash, column->collation_deterministic ? 1U : 0U);
        if (!vps_hash_string(&hash, columns, &column->name) ||
            !vps_hash_string(&hash, columns, &column->type_namespace) ||
            !vps_hash_string(&hash, columns, &column->type_name) ||
            !vps_hash_string(&hash, columns,
                             &column->domain_base_namespace) ||
            !vps_hash_string(&hash, columns, &column->domain_base_name) ||
            !vps_hash_string(&hash, columns, &column->collation_name) ||
            !vps_hash_string(&hash, columns,
                             &column->default_expression_hash) ||
            !vps_hash_string(&hash, columns,
                             &column->domain_default_hash) ||
            !vps_hash_string(&hash, columns,
                             &column->domain_constraint_hash)) return 0;
    }
    *component = hash;
    return 1;
}

static uint64_t vps_key_component(const VpsKeyMetadata *key)
{
    uint64_t hash = UINT64_C(7809847782465536322);
    size_t index;
    vps_hash_u32(&hash, (uint32_t)key->source);
    vps_hash_u32(&hash, key->index_oid);
    vps_hash_u64(&hash, (uint64_t)key->column_count);
    vps_hash_u32(&hash, key->nulls_not_distinct ? 1U : 0U);
    vps_hash_u32(&hash, key->read_only ? 1U : 0U);
    for (index = 0U; index < key->column_count; ++index)
        vps_hash_u32(&hash, (uint32_t)key->attribute_numbers[index]);
    return hash;
}

static uint64_t vps_policy_component(const VpsRelationPolicyMetadata *policy)
{
    uint64_t hash = UINT64_C(9650029242287828579);
    size_t index;
    vps_hash_u64(&hash, (uint64_t)policy->parent_count);
    for (index = 0U; index < policy->parent_count; ++index)
        vps_hash_u32(&hash, policy->parent_oids[index]);
    vps_hash_u32(&hash, (uint32_t)(unsigned char)policy->partition_strategy);
    vps_hash_u64(&hash, (uint64_t)policy->partition_attribute_count);
    for (index = 0U; index < policy->partition_attribute_count; ++index)
        vps_hash_u32(&hash,
                     (uint32_t)policy->partition_attribute_numbers[index]);
    vps_hash_u32(&hash, policy->row_security ? 1U : 0U);
    vps_hash_u32(&hash, policy->force_row_security ? 1U : 0U);
    vps_hash_u32(&hash, (uint32_t)policy->write_policy);
    return hash;
}

static VpsMetadataResult vps_codec_component(
    const VpsTypeRegistry *registry,
    const VpsColumnSet *columns,
    uint64_t *component)
{
    uint64_t hash = UINT64_C(14695981039346656037) ^ UINT64_C(0x434f444543);
    size_t index;
    vps_hash_u32(&hash, registry->version);
    for (index = 0U; index < columns->column_count; ++index) {
        VpsTypeSelection selection;
        VpsMetadataResult result = vps_type_registry_select(
            registry, columns, index, &selection);
        if (result != VPS_METADATA_OK) return result;
        vps_hash_u32(&hash, selection.declared_type_oid);
        vps_hash_u32(&hash, selection.effective_type_oid);
        vps_hash_u32(&hash, (uint32_t)selection.codec);
        vps_hash_u32(&hash, selection.capabilities);
        vps_hash_u32(&hash, selection.domain ? 1U : 0U);
    }
    *component = hash;
    return VPS_METADATA_OK;
}

static uint64_t vps_spatial_component(const VpsSchemaSpatialMetadata *spatial)
{
    uint64_t hash = UINT64_C(1099511628211) ^ UINT64_C(0x5350415449414c);
    vps_hash_u32(&hash, spatial->metadata_version);
    vps_hash_u32(&hash, spatial->type_oid);
    vps_hash_u32(&hash, spatial->extension_namespace_oid);
    vps_hash_u32(&hash, (uint32_t)spatial->srid);
    vps_hash_u32(&hash, spatial->dimensions);
    vps_hash_u32(&hash, spatial->format);
    return hash;
}

static void vps_schema_hex(const unsigned char *bytes, char *hex)
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0U; index < VPS_SCHEMA_FINGERPRINT_BYTES; ++index) {
        hex[index * 2U] = digits[(bytes[index] >> 4) & 0x0fU];
        hex[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    hex[VPS_SCHEMA_FINGERPRINT_HEX_LENGTH] = '\0';
}

static void vps_schema_log(VpsLogger *logger,
                           const VpsSchemaFingerprint *fingerprint)
{
    VpsLogEvent event;
    static const char operation[] = "schema_fingerprint";
    static const char status[] = "passed";
    if (logger == NULL || fingerprint == NULL) return;
    if (vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 sizeof(status) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_QUERY_FINGERPRINT,
                                 fingerprint->hex,
                                 VPS_SCHEMA_FINGERPRINT_HEX_LENGTH) !=
            VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_FORMAT_VERSION,
                                 fingerprint->version) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_RESULT_FIELD_COUNT,
                                 (uint64_t)fingerprint->input_column_count) !=
            VPS_LOG_OK) return;
    vps_logger_emit(logger, &event);
}

VpsMetadataResult vps_schema_fingerprint_build(
    const VpsSchemaFingerprintInput *input,
    VpsLogger *logger,
    VpsSchemaFingerprint *fingerprint)
{
    static const uint64_t seeds[4] = {
        UINT64_C(14695981039346656037), UINT64_C(1099511628211),
        UINT64_C(7809847782465536322), UINT64_C(9650029242287828579)};
    VpsSchemaFingerprint candidate;
    uint64_t components[6];
    size_t lane;
    size_t byte;
    VpsMetadataResult result;
    if (input == NULL || input->relation == NULL ||
        !input->relation->initialized || input->columns == NULL ||
        !input->columns->initialized || input->key == NULL ||
        input->policy == NULL || input->type_registry == NULL ||
        !input->type_registry->initialized || fingerprint == NULL)
        return VPS_METADATA_INVALID_ARGUMENT;
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.version = VPS_SCHEMA_FINGERPRINT_VERSION;
    candidate.relation_hash = vps_relation_component(input->relation);
    if (!vps_layout_component(input->columns, &candidate.layout_hash))
        return VPS_METADATA_INVALID_RESULT;
    candidate.key_hash = vps_key_component(input->key);
    candidate.policy_hash = vps_policy_component(input->policy);
    result = vps_codec_component(input->type_registry, input->columns,
                                 &candidate.codec_hash);
    if (result != VPS_METADATA_OK) return result;
    candidate.spatial_hash = vps_spatial_component(&input->spatial);
    candidate.input_column_count = input->columns->column_count;
    components[0] = candidate.relation_hash;
    components[1] = candidate.layout_hash;
    components[2] = candidate.key_hash;
    components[3] = candidate.policy_hash;
    components[4] = candidate.codec_hash;
    components[5] = candidate.spatial_hash;
    for (lane = 0U; lane < 4U; ++lane) {
        uint64_t hash = seeds[lane] ^ (uint64_t)candidate.version;
        for (byte = 0U; byte < sizeof(components) / sizeof(components[0]); ++byte)
            vps_hash_u64(&hash, components[byte]);
        for (byte = 0U; byte < 8U; ++byte)
            candidate.bytes[lane * 8U + byte] =
                (unsigned char)((hash >> (byte * 8U)) & UINT64_C(0xff));
    }
    vps_schema_hex(candidate.bytes, candidate.hex);
    *fingerprint = candidate;
    vps_schema_log(logger, fingerprint);
    return VPS_METADATA_OK;
}

VpsSchemaChangeClass vps_schema_fingerprint_compare(
    const VpsSchemaFingerprint *previous,
    const VpsSchemaFingerprint *current)
{
    if (previous == NULL || current == NULL ||
        previous->version != VPS_SCHEMA_FINGERPRINT_VERSION ||
        current->version != VPS_SCHEMA_FINGERPRINT_VERSION)
        return VPS_SCHEMA_CHANGE_UNKNOWN;
    if (memcmp(previous->bytes, current->bytes,
               VPS_SCHEMA_FINGERPRINT_BYTES) == 0)
        return VPS_SCHEMA_CHANGE_NONE;
    if (previous->relation_hash != current->relation_hash)
        return VPS_SCHEMA_CHANGE_RELATION_IDENTITY;
    if (previous->layout_hash != current->layout_hash)
        return VPS_SCHEMA_CHANGE_COLUMN_LAYOUT;
    if (previous->key_hash != current->key_hash)
        return VPS_SCHEMA_CHANGE_KEY;
    if (previous->policy_hash != current->policy_hash)
        return VPS_SCHEMA_CHANGE_RELATION_POLICY;
    if (previous->codec_hash != current->codec_hash)
        return VPS_SCHEMA_CHANGE_CODEC_REGISTRY;
    if (previous->spatial_hash != current->spatial_hash)
        return VPS_SCHEMA_CHANGE_SPATIAL;
    return VPS_SCHEMA_CHANGE_UNKNOWN;
}

const char *vps_schema_change_class_name(VpsSchemaChangeClass change_class)
{
    switch (change_class) {
        case VPS_SCHEMA_CHANGE_NONE: return "none";
        case VPS_SCHEMA_CHANGE_RELATION_IDENTITY: return "relation_identity";
        case VPS_SCHEMA_CHANGE_COLUMN_LAYOUT: return "column_layout";
        case VPS_SCHEMA_CHANGE_KEY: return "key";
        case VPS_SCHEMA_CHANGE_RELATION_POLICY: return "relation_policy";
        case VPS_SCHEMA_CHANGE_CODEC_REGISTRY: return "codec_registry";
        case VPS_SCHEMA_CHANGE_SPATIAL: return "spatial";
        case VPS_SCHEMA_CHANGE_UNKNOWN: return "unknown";
        default: return "unknown";
    }
}
