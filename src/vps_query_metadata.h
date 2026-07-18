#ifndef VPS_QUERY_METADATA_H
#define VPS_QUERY_METADATA_H

#include "vps_query_indexes.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_QUERY_METADATA_FORMAT_VERSION UINT32_C(1)
#define VPS_QUERY_METADATA_MAX_KEY_COLUMNS 32U

typedef enum VpsQueryMetadataResult {
    VPS_QUERY_METADATA_OK = 0,
    VPS_QUERY_METADATA_INVALID_ARGUMENT = 1,
    VPS_QUERY_METADATA_DUPLICATE_ALIAS = 2,
    VPS_QUERY_METADATA_UNKNOWN_KEY_COLUMN = 3,
    VPS_QUERY_METADATA_DUPLICATE_KEY_COLUMN = 4,
    VPS_QUERY_METADATA_INVALID_KEY_SYNTAX = 5,
    VPS_QUERY_METADATA_INVALID_INDEX = 6,
    VPS_QUERY_METADATA_OUT_OF_MEMORY = 7
} VpsQueryMetadataResult;

typedef enum VpsQuerySpatialKind {
    VPS_QUERY_SPATIAL_NONE = 0,
    VPS_QUERY_SPATIAL_GEOMETRY = 1,
    VPS_QUERY_SPATIAL_GEOGRAPHY = 2
} VpsQuerySpatialKind;

typedef enum VpsQueryMaterializationMode {
    VPS_QUERY_MATERIALIZATION_OFF = 0,
    VPS_QUERY_MATERIALIZATION_MEMORY = 1,
    VPS_QUERY_MATERIALIZATION_TEMP = 2
} VpsQueryMaterializationMode;

typedef struct VpsQueryColumnPolicy {
    uint32_t collation_oid;
    VpsQuerySpatialKind spatial_kind;
    int32_t spatial_srid;
    uint8_t spatial_dimensions;
} VpsQueryColumnPolicy;

typedef struct VpsQueryResultColumn {
    char name[VPS_CLIENT_MAX_FIELD_NAME_BYTES + 1U];
    size_t name_length;
    uint64_t canonical_name_hash;
    uint32_t type_oid;
    int32_t type_modifier;
    uint32_t origin_relation_oid;
    int32_t origin_attribute_number;
    uint32_t collation_oid;
    VpsQuerySpatialKind spatial_kind;
    int32_t spatial_srid;
    uint8_t spatial_dimensions;
} VpsQueryResultColumn;

typedef struct VpsQueryMetadataPolicy {
    const VpsQueryColumnPolicy *columns;
    size_t column_count;
    const char *key_columns;
    size_t key_columns_length;
    const char *query_indexes;
    size_t query_indexes_length;
    VpsQueryMaterializationMode materialization;
} VpsQueryMetadataPolicy;

typedef struct VpsQueryResultMetadata {
    VpsAllocator allocator;
    VpsQueryResultColumn *columns;
    size_t column_count;
    size_t allocation_size;
    uint16_t key_columns[VPS_QUERY_METADATA_MAX_KEY_COLUMNS];
    size_t key_column_count;
    VpsQueryIndexSet indexes;
    VpsQueryMaterializationMode materialization;
    VpsLogger *logger;
    int initialized;
} VpsQueryResultMetadata;

VpsQueryMetadataResult vps_query_result_metadata_init(
    VpsQueryResultMetadata *metadata,
    const VpsAllocator *allocator,
    VpsLogger *logger);
VpsQueryMetadataResult vps_query_result_metadata_build(
    VpsQueryResultMetadata *metadata,
    const VpsQueryDescribeResult *described,
    const VpsQueryMetadataPolicy *policy);
void vps_query_result_metadata_cleanup(VpsQueryResultMetadata *metadata);

const char *vps_query_metadata_result_name(VpsQueryMetadataResult result);

#endif
