#include "vps_query_fingerprint.h"

#include <string.h>

#define VPS_QUERY_FNV_PRIME UINT64_C(1099511628211)

static void vps_query_hash_bytes(uint64_t *hash,
                                 const void *value,
                                 size_t length)
{
    const unsigned char *bytes = (const unsigned char *)value;
    size_t index;
    for (index = 0U; index < length; ++index) {
        *hash = (*hash ^ (uint64_t)bytes[index]) * VPS_QUERY_FNV_PRIME;
    }
}

static void vps_query_hash_u32(uint64_t *hash, uint32_t value)
{
    unsigned char bytes[4];
    size_t index;
    for (index = 0U; index < sizeof(bytes); ++index) {
        bytes[index] = (unsigned char)((value >> (index * 8U)) & 0xffU);
    }
    vps_query_hash_bytes(hash, bytes, sizeof(bytes));
}

static void vps_query_hash_u64(uint64_t *hash, uint64_t value)
{
    unsigned char bytes[8];
    size_t index;
    for (index = 0U; index < sizeof(bytes); ++index) {
        bytes[index] =
            (unsigned char)((value >> (index * 8U)) & UINT64_C(0xff));
    }
    vps_query_hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t vps_query_layout_hash(const VpsQueryResultMetadata *metadata)
{
    uint64_t hash = UINT64_C(1099511628211) ^ UINT64_C(0x4c41594f5554);
    size_t index;
    vps_query_hash_u64(&hash, (uint64_t)metadata->column_count);
    for (index = 0U; index < metadata->column_count; ++index) {
        const VpsQueryResultColumn *column = &metadata->columns[index];
        vps_query_hash_u64(&hash, column->canonical_name_hash);
        vps_query_hash_u32(&hash, column->type_oid);
        vps_query_hash_u32(&hash, (uint32_t)column->type_modifier);
        vps_query_hash_u32(&hash, column->origin_relation_oid);
        vps_query_hash_u32(&hash,
                           (uint32_t)column->origin_attribute_number);
        vps_query_hash_u32(&hash, column->collation_oid);
        vps_query_hash_u32(&hash, (uint32_t)column->spatial_kind);
        vps_query_hash_u32(&hash, (uint32_t)column->spatial_srid);
        vps_query_hash_u32(&hash, (uint32_t)column->spatial_dimensions);
    }
    return hash;
}

static uint64_t vps_query_key_hash(const VpsQueryResultMetadata *metadata)
{
    uint64_t hash = UINT64_C(7809847782465536322);
    size_t index;
    vps_query_hash_u64(&hash, (uint64_t)metadata->key_column_count);
    for (index = 0U; index < metadata->key_column_count; ++index) {
        vps_query_hash_u32(&hash, metadata->key_columns[index]);
    }
    return hash;
}

static uint64_t vps_query_indexes_hash(const VpsQueryResultMetadata *metadata)
{
    uint64_t hash = UINT64_C(9650029242287828579);
    size_t index;
    vps_query_hash_u32(&hash, metadata->indexes.format_version);
    vps_query_hash_u64(&hash, (uint64_t)metadata->indexes.index_count);
    for (index = 0U; index < metadata->indexes.index_count; ++index) {
        const VpsQueryIndexDefinition *definition =
            &metadata->indexes.indexes[index];
        size_t column;
        vps_query_hash_u64(&hash, definition->name_hash);
        vps_query_hash_u64(&hash, (uint64_t)definition->column_count);
        for (column = 0U; column < definition->column_count; ++column) {
            vps_query_hash_u32(&hash, definition->columns[column]);
        }
    }
    return hash;
}

static void vps_query_fingerprint_hex(const unsigned char *bytes, char *hex)
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0U; index < VPS_QUERY_FINGERPRINT_BYTES; ++index) {
        hex[index * 2U] = digits[(bytes[index] >> 4) & 0x0fU];
        hex[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    hex[VPS_QUERY_FINGERPRINT_HEX_LENGTH] = '\0';
}

static void vps_query_fingerprint_log(VpsLogger *logger,
                                      const VpsQueryFingerprint *fingerprint)
{
    VpsLogEvent event;
    if (logger == NULL || vps_log_event_init(&event, VPS_LOG_LEVEL_DEBUG) !=
                              VPS_LOG_OK) return;
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION,
                                   "query_fingerprint", 17U);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS,
                                   "passed", 6U);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_QUERY_FINGERPRINT,
                                   fingerprint->hex,
                                   VPS_QUERY_FINGERPRINT_HEX_LENGTH);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_FORMAT_VERSION,
                                   fingerprint->version);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_RESULT_FIELD_COUNT,
                                   (uint64_t)fingerprint->input_column_count);
    vps_logger_emit(logger, &event);
}

VpsQueryMetadataResult vps_query_fingerprint_build(
    const VpsQueryFingerprintInput *input,
    VpsLogger *logger,
    VpsQueryFingerprint *fingerprint)
{
    static const uint64_t seeds[4] = {
        UINT64_C(14695981039346656037), UINT64_C(1099511628211),
        UINT64_C(7809847782465536322), UINT64_C(9650029242287828579)
    };
    VpsQueryFingerprint candidate;
    uint64_t components[7];
    size_t lane;
    size_t index;
    if (input == NULL || input->metadata == NULL ||
        !input->metadata->initialized || input->metadata->columns == NULL ||
        input->metadata->column_count == 0U ||
        input->normalized_query_hash == 0U ||
        input->wrapper_format_version == 0U ||
        input->codec_registry_version == 0U || fingerprint == NULL) {
        return VPS_QUERY_METADATA_INVALID_ARGUMENT;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.version = VPS_QUERY_FINGERPRINT_VERSION;
    candidate.source_hash = input->normalized_query_hash;
    candidate.profile_hash = UINT64_C(1469598103934665603);
    vps_query_hash_u64(&candidate.profile_hash, input->profile_version);
    vps_query_hash_u64(&candidate.profile_hash, input->profile_generation);
    candidate.layout_hash = vps_query_layout_hash(input->metadata);
    candidate.key_hash = vps_query_key_hash(input->metadata);
    candidate.indexes_hash = vps_query_indexes_hash(input->metadata);
    candidate.policy_hash = UINT64_C(1099511628211);
    vps_query_hash_u32(&candidate.policy_hash,
                       (uint32_t)input->metadata->materialization);
    candidate.wrapper_hash = UINT64_C(7809847782465536322);
    vps_query_hash_u32(&candidate.wrapper_hash,
                       input->wrapper_format_version);
    vps_query_hash_u32(&candidate.wrapper_hash,
                       input->codec_registry_version);
    candidate.input_column_count = input->metadata->column_count;
    components[0] = candidate.source_hash;
    components[1] = candidate.profile_hash;
    components[2] = candidate.layout_hash;
    components[3] = candidate.key_hash;
    components[4] = candidate.indexes_hash;
    components[5] = candidate.policy_hash;
    components[6] = candidate.wrapper_hash;
    for (lane = 0U; lane < 4U; ++lane) {
        uint64_t hash = seeds[lane] ^ candidate.version;
        for (index = 0U; index < sizeof(components) / sizeof(components[0]);
             ++index) vps_query_hash_u64(&hash, components[index]);
        for (index = 0U; index < 8U; ++index) {
            candidate.bytes[lane * 8U + index] =
                (unsigned char)((hash >> (index * 8U)) & UINT64_C(0xff));
        }
    }
    vps_query_fingerprint_hex(candidate.bytes, candidate.hex);
    *fingerprint = candidate;
    vps_query_fingerprint_log(logger, fingerprint);
    return VPS_QUERY_METADATA_OK;
}

VpsQueryChangeClass vps_query_fingerprint_compare(
    const VpsQueryFingerprint *previous,
    const VpsQueryFingerprint *current)
{
    if (previous == NULL || current == NULL || previous->version == 0U ||
        current->version == 0U) return VPS_QUERY_CHANGE_UNKNOWN;
    if (previous->version != current->version) return VPS_QUERY_CHANGE_WRAPPER;
    if (previous->source_hash != current->source_hash)
        return VPS_QUERY_CHANGE_SOURCE;
    if (previous->profile_hash != current->profile_hash)
        return VPS_QUERY_CHANGE_PROFILE;
    if (previous->layout_hash != current->layout_hash)
        return VPS_QUERY_CHANGE_LAYOUT;
    if (previous->key_hash != current->key_hash) return VPS_QUERY_CHANGE_KEY;
    if (previous->indexes_hash != current->indexes_hash)
        return VPS_QUERY_CHANGE_INDEXES;
    if (previous->policy_hash != current->policy_hash)
        return VPS_QUERY_CHANGE_POLICY;
    if (previous->wrapper_hash != current->wrapper_hash)
        return VPS_QUERY_CHANGE_WRAPPER;
    return memcmp(previous->bytes, current->bytes,
                  VPS_QUERY_FINGERPRINT_BYTES) == 0
               ? VPS_QUERY_CHANGE_NONE : VPS_QUERY_CHANGE_UNKNOWN;
}

const char *vps_query_change_class_name(VpsQueryChangeClass change_class)
{
    static const char *const names[] = {
        "none", "source", "profile", "layout", "key", "indexes",
        "policy", "wrapper", "unknown"
    };
    if (change_class < VPS_QUERY_CHANGE_NONE ||
        change_class > VPS_QUERY_CHANGE_UNKNOWN) return "unknown";
    return names[(size_t)change_class];
}
