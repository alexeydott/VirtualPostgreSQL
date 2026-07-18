#ifndef VPS_QUERY_INDEXES_H
#define VPS_QUERY_INDEXES_H

#include "vps_query_validation.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_QUERY_INDEXES_FORMAT_VERSION UINT32_C(1)
#define VPS_QUERY_INDEXES_MAX_BYTES 8192U
#define VPS_QUERY_INDEX_MAX_COUNT 64U
#define VPS_QUERY_INDEX_MAX_COLUMNS 32U

typedef enum VpsQueryIndexesResult {
    VPS_QUERY_INDEXES_OK = 0,
    VPS_QUERY_INDEXES_INVALID_ARGUMENT = 1,
    VPS_QUERY_INDEXES_LIMIT_EXCEEDED = 2,
    VPS_QUERY_INDEXES_INVALID_SYNTAX = 3,
    VPS_QUERY_INDEXES_DUPLICATE_NAME = 4,
    VPS_QUERY_INDEXES_UNKNOWN_COLUMN = 5,
    VPS_QUERY_INDEXES_DUPLICATE_COLUMN = 6
} VpsQueryIndexesResult;

typedef struct VpsQueryIndexDefinition {
    char name[VPS_CLIENT_MAX_FIELD_NAME_BYTES + 1U];
    size_t name_length;
    uint64_t name_hash;
    uint16_t columns[VPS_QUERY_INDEX_MAX_COLUMNS];
    size_t column_count;
} VpsQueryIndexDefinition;

typedef struct VpsQueryIndexSet {
    uint32_t format_version;
    VpsQueryIndexDefinition indexes[VPS_QUERY_INDEX_MAX_COUNT];
    size_t index_count;
} VpsQueryIndexSet;

VpsQueryIndexesResult vps_query_indexes_parse(
    const char *definition,
    size_t definition_length,
    const VpsQueryDescribeResult *described,
    VpsQueryIndexSet *indexes);

const char *vps_query_indexes_result_name(VpsQueryIndexesResult result);

#endif
