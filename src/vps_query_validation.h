#ifndef VPS_QUERY_VALIDATION_H
#define VPS_QUERY_VALIDATION_H

#include "vps_client.h"
#include "vps_query_source.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_QUERY_WRAPPER_FORMAT_VERSION UINT32_C(1)

typedef enum VpsQueryValidationResult {
    VPS_QUERY_VALIDATION_OK = 0,
    VPS_QUERY_VALIDATION_INVALID_ARGUMENT = 1,
    VPS_QUERY_VALIDATION_SCAN_REJECTED = 2,
    VPS_QUERY_VALIDATION_LIMIT_EXCEEDED = 3,
    VPS_QUERY_VALIDATION_OUT_OF_MEMORY = 4,
    VPS_QUERY_VALIDATION_CLIENT_ERROR = 5,
    VPS_QUERY_VALIDATION_INVALID_DESCRIPTOR = 6
} VpsQueryValidationResult;

typedef struct VpsQueryDescribeField {
    char name[VPS_CLIENT_MAX_FIELD_NAME_BYTES + 1U];
    size_t name_length;
    uint32_t type_oid;
    int32_t type_modifier;
    uint32_t origin_relation_oid;
    int32_t origin_attribute_number;
    VpsClientValueFormat format;
} VpsQueryDescribeField;

typedef struct VpsQueryDescribeResult {
    VpsAllocator allocator;
    VpsQueryDescribeField *fields;
    size_t field_count;
    size_t allocation_size;
    uint64_t wrapper_query_fingerprint;
    int initialized;
} VpsQueryDescribeResult;

/* Owns wrapper bytes and borrows logger. Cleanup is idempotent. */
typedef struct VpsQueryValidation {
    VpsBuffer wrapper;
    VpsClientStatementSpec statement_spec;
    VpsQuerySourceAnalysis source_analysis;
    VpsLogger *logger;
    int initialized;
} VpsQueryValidation;

VpsQueryValidationResult vps_query_validation_init(
    VpsQueryValidation *validation,
    const VpsAllocator *allocator,
    const char *query,
    size_t query_length,
    uint64_t timeout_ms,
    VpsLogger *logger);
void vps_query_validation_cleanup(VpsQueryValidation *validation);
const VpsClientStatementSpec *vps_query_validation_statement_spec(
    const VpsQueryValidation *validation);

VpsQueryValidationResult vps_query_describe_result_init(
    VpsQueryDescribeResult *result,
    const VpsAllocator *allocator);
VpsQueryValidationResult vps_query_validation_collect(
    const VpsClientStatement *statement,
    VpsQueryDescribeResult *result,
    VpsError *error);
void vps_query_describe_result_cleanup(VpsQueryDescribeResult *result);

const char *vps_query_validation_result_name(VpsQueryValidationResult result);

#endif
