#include "vps_query_metadata.h"

#include <ctype.h>
#include <string.h>

#define VPS_QUERY_METADATA_HASH_OFFSET UINT64_C(1469598103934665603)
#define VPS_QUERY_METADATA_HASH_PRIME UINT64_C(1099511628211)

static int vps_query_metadata_name_equal(const char *left,
                                         size_t left_length,
                                         const char *right,
                                         size_t right_length)
{
    size_t index;
    if (left_length != right_length) return 0;
    for (index = 0U; index < left_length; ++index) {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (a < 0x80U) a = (unsigned char)tolower(a);
        if (b < 0x80U) b = (unsigned char)tolower(b);
        if (a != b) return 0;
    }
    return 1;
}

static uint64_t vps_query_metadata_name_hash(const char *name, size_t length)
{
    uint64_t hash = VPS_QUERY_METADATA_HASH_OFFSET;
    size_t index;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)name[index];
        if (value < 0x80U) value = (unsigned char)tolower(value);
        hash = (hash ^ (uint64_t)value) * VPS_QUERY_METADATA_HASH_PRIME;
    }
    return hash;
}

static void vps_query_metadata_log(VpsQueryResultMetadata *metadata,
                                   VpsQueryMetadataResult result)
{
    VpsLogEvent event;
    const char *status = vps_query_metadata_result_name(result);
    if (metadata->logger == NULL ||
        vps_log_event_init(&event, result == VPS_QUERY_METADATA_OK
                                      ? VPS_LOG_LEVEL_DEBUG
                                      : VPS_LOG_LEVEL_WARN) != VPS_LOG_OK) {
        return;
    }
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION,
                                   "query_metadata", 14U);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS,
                                   status, strlen(status));
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_RESULT_FIELD_COUNT,
                                   (uint64_t)metadata->column_count);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_KEY_COUNT,
                                   (uint64_t)metadata->key_column_count);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_INDEX_OID,
                                   (uint64_t)metadata->indexes.index_count);
    vps_logger_emit(metadata->logger, &event);
}

VpsQueryMetadataResult vps_query_result_metadata_init(
    VpsQueryResultMetadata *metadata,
    const VpsAllocator *allocator,
    VpsLogger *logger)
{
    if (metadata == NULL || !vps_allocator_is_valid(allocator)) {
        return VPS_QUERY_METADATA_INVALID_ARGUMENT;
    }
    (void)memset(metadata, 0, sizeof(*metadata));
    metadata->allocator = *allocator;
    metadata->logger = logger;
    metadata->initialized = 1;
    return VPS_QUERY_METADATA_OK;
}

static int vps_query_metadata_find_column(
    const VpsQueryResultMetadata *metadata,
    const char *name,
    size_t name_length,
    uint16_t *column)
{
    size_t index;
    for (index = 0U; index < metadata->column_count; ++index) {
        if (vps_query_metadata_name_equal(
                name, name_length, metadata->columns[index].name,
                metadata->columns[index].name_length)) {
            *column = (uint16_t)index;
            return 1;
        }
    }
    return 0;
}

static VpsQueryMetadataResult vps_query_metadata_parse_keys(
    VpsQueryResultMetadata *metadata,
    const char *keys,
    size_t keys_length)
{
    size_t position = 0U;
    if (keys_length == 0U) return VPS_QUERY_METADATA_OK;
    if (keys == NULL || keys_length > VPS_QUERY_INDEXES_MAX_BYTES) {
        return VPS_QUERY_METADATA_INVALID_ARGUMENT;
    }
    while (position < keys_length) {
        size_t end = position;
        size_t start;
        uint16_t column;
        size_t existing;
        while (end < keys_length && keys[end] != ',') ++end;
        start = position;
        while (start < end && isspace((unsigned char)keys[start]) != 0) {
            ++start;
        }
        while (end > start && isspace((unsigned char)keys[end - 1U]) != 0) {
            --end;
        }
        if (start == end ||
            metadata->key_column_count >= VPS_QUERY_METADATA_MAX_KEY_COLUMNS) {
            return VPS_QUERY_METADATA_INVALID_KEY_SYNTAX;
        }
        if (!vps_query_metadata_find_column(metadata, keys + start,
                                            end - start, &column)) {
            return VPS_QUERY_METADATA_UNKNOWN_KEY_COLUMN;
        }
        for (existing = 0U; existing < metadata->key_column_count; ++existing) {
            if (metadata->key_columns[existing] == column) {
                return VPS_QUERY_METADATA_DUPLICATE_KEY_COLUMN;
            }
        }
        metadata->key_columns[metadata->key_column_count++] = column;
        while (end < keys_length && keys[end] != ',') ++end;
        position = end + 1U;
        if (position == keys_length) {
            return VPS_QUERY_METADATA_INVALID_KEY_SYNTAX;
        }
    }
    return VPS_QUERY_METADATA_OK;
}

VpsQueryMetadataResult vps_query_result_metadata_build(
    VpsQueryResultMetadata *metadata,
    const VpsQueryDescribeResult *described,
    const VpsQueryMetadataPolicy *policy)
{
    size_t allocation_size;
    size_t index;
    VpsQueryMetadataResult result = VPS_QUERY_METADATA_OK;
    if (metadata == NULL || !metadata->initialized || metadata->columns != NULL ||
        described == NULL || !described->initialized ||
        described->fields == NULL || described->field_count == 0U ||
        policy == NULL ||
        (policy->columns != NULL &&
         policy->column_count != described->field_count) ||
        (policy->columns == NULL && policy->column_count != 0U) ||
        policy->materialization < VPS_QUERY_MATERIALIZATION_OFF ||
        policy->materialization > VPS_QUERY_MATERIALIZATION_TEMP) {
        return VPS_QUERY_METADATA_INVALID_ARGUMENT;
    }
    if (vps_size_multiply(described->field_count,
                          sizeof(*metadata->columns), &allocation_size) !=
            VPS_MEMORY_OK ||
        vps_memory_allocate(&metadata->allocator, allocation_size,
                            (void **)&metadata->columns) != VPS_MEMORY_OK) {
        return VPS_QUERY_METADATA_OUT_OF_MEMORY;
    }
    (void)memset(metadata->columns, 0, allocation_size);
    metadata->allocation_size = allocation_size;
    metadata->column_count = described->field_count;
    metadata->materialization = policy->materialization;
    for (index = 0U; index < metadata->column_count; ++index) {
        const VpsQueryDescribeField *source = &described->fields[index];
        VpsQueryResultColumn *target = &metadata->columns[index];
        size_t prior;
        for (prior = 0U; prior < index; ++prior) {
            if (vps_query_metadata_name_equal(
                    source->name, source->name_length,
                    metadata->columns[prior].name,
                    metadata->columns[prior].name_length)) {
                result = VPS_QUERY_METADATA_DUPLICATE_ALIAS;
                goto fail;
            }
        }
        (void)memcpy(target->name, source->name, source->name_length + 1U);
        target->name_length = source->name_length;
        target->canonical_name_hash = vps_query_metadata_name_hash(
            source->name, source->name_length);
        target->type_oid = source->type_oid;
        target->type_modifier = source->type_modifier;
        target->origin_relation_oid = source->origin_relation_oid;
        target->origin_attribute_number = source->origin_attribute_number;
        if (policy->columns != NULL) {
            target->collation_oid = policy->columns[index].collation_oid;
            target->spatial_kind = policy->columns[index].spatial_kind;
            target->spatial_srid = policy->columns[index].spatial_srid;
            target->spatial_dimensions =
                policy->columns[index].spatial_dimensions;
        }
    }
    result = vps_query_metadata_parse_keys(
        metadata, policy->key_columns, policy->key_columns_length);
    if (result != VPS_QUERY_METADATA_OK) goto fail;
    if (vps_query_indexes_parse(policy->query_indexes,
                                policy->query_indexes_length,
                                described, &metadata->indexes) !=
        VPS_QUERY_INDEXES_OK) {
        result = VPS_QUERY_METADATA_INVALID_INDEX;
        goto fail;
    }
    vps_query_metadata_log(metadata, VPS_QUERY_METADATA_OK);
    return VPS_QUERY_METADATA_OK;

fail:
    vps_query_metadata_log(metadata, result);
    vps_memory_release(&metadata->allocator, (void **)&metadata->columns,
                       metadata->allocation_size);
    metadata->allocation_size = 0U;
    metadata->column_count = 0U;
    metadata->key_column_count = 0U;
    (void)memset(&metadata->indexes, 0, sizeof(metadata->indexes));
    return result;
}

void vps_query_result_metadata_cleanup(VpsQueryResultMetadata *metadata)
{
    if (metadata == NULL || !metadata->initialized) return;
    vps_memory_release(&metadata->allocator, (void **)&metadata->columns,
                       metadata->allocation_size);
    metadata->allocation_size = 0U;
    metadata->column_count = 0U;
    metadata->key_column_count = 0U;
    (void)memset(&metadata->indexes, 0, sizeof(metadata->indexes));
}

const char *vps_query_metadata_result_name(VpsQueryMetadataResult result)
{
    static const char *const names[] = {
        "ok", "invalid_argument", "duplicate_alias", "unknown_key_column",
        "duplicate_key_column", "invalid_key_syntax", "invalid_index",
        "out_of_memory"
    };
    if (result < VPS_QUERY_METADATA_OK ||
        result > VPS_QUERY_METADATA_OUT_OF_MEMORY) return "unknown";
    return names[(size_t)result];
}
