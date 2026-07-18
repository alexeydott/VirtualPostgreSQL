#ifndef VPS_PLANNER_H
#define VPS_PLANNER_H

#include "vps_logging.h"
#include "vps_memory.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_PLAN_FORMAT_VERSION UINT32_C(1)
#define VPS_PLAN_MAX_COLUMNS 1024U
#define VPS_PLAN_MAX_CONSTRAINTS 64U
#define VPS_PLAN_MAX_ORDER_TERMS 32U
#define VPS_PLAN_MAX_ENCODED_BYTES 65536U
#define VPS_PLAN_DEFAULT_IN_LIMIT 4096U

#define VPS_PLAN_CAP_EQUALITY UINT32_C(1)
#define VPS_PLAN_CAP_ORDER UINT32_C(2)
#define VPS_PLAN_CAP_EXACT UINT32_C(4)
#define VPS_PLAN_CAP_BINARY UINT32_C(8)

#define VPS_PLAN_FLAG_ORDER_CONSUMED UINT32_C(1)
#define VPS_PLAN_FLAG_UNIQUE UINT32_C(2)
#define VPS_PLAN_FLAG_HAS_RECHECK UINT32_C(4)
#define VPS_PLAN_FLAG_LIMIT_CONSUMED UINT32_C(8)
#define VPS_PLAN_FLAG_OFFSET_CONSUMED UINT32_C(16)

#define VPS_PLAN_CONSTRAINT_EXACT UINT16_C(1)
#define VPS_PLAN_CONSTRAINT_RECHECK UINT16_C(2)
#define VPS_PLAN_CONSTRAINT_IN UINT16_C(4)
#define VPS_PLAN_CONSTRAINT_KEY UINT16_C(8)
#define VPS_PLAN_CONSTRAINT_NULL_SAFE UINT16_C(16)

typedef enum VpsPlannerResult {
    VPS_PLANNER_OK = 0,
    VPS_PLANNER_INVALID_ARGUMENT = 1,
    VPS_PLANNER_LIMIT_EXCEEDED = 2,
    VPS_PLANNER_INVALID_PLAN = 3,
    VPS_PLANNER_VERSION_MISMATCH = 4,
    VPS_PLANNER_FINGERPRINT_MISMATCH = 5,
    VPS_PLANNER_OUT_OF_MEMORY = 6
} VpsPlannerResult;

typedef enum VpsPlanOperator {
    VPS_PLAN_OP_EQ = 1,
    VPS_PLAN_OP_NE = 2,
    VPS_PLAN_OP_LT = 3,
    VPS_PLAN_OP_LE = 4,
    VPS_PLAN_OP_GT = 5,
    VPS_PLAN_OP_GE = 6,
    VPS_PLAN_OP_IS_NULL = 7,
    VPS_PLAN_OP_IS_NOT_NULL = 8,
    VPS_PLAN_OP_IN = 9,
    VPS_PLAN_OP_LIMIT = 10,
    VPS_PLAN_OP_OFFSET = 11
} VpsPlanOperator;

typedef enum VpsPlanValueClass {
    VPS_PLAN_VALUE_UNKNOWN = 0,
    VPS_PLAN_VALUE_NULL = 1,
    VPS_PLAN_VALUE_INTEGER = 2,
    VPS_PLAN_VALUE_REAL = 3,
    VPS_PLAN_VALUE_TEXT = 4,
    VPS_PLAN_VALUE_BLOB = 5
} VpsPlanValueClass;

typedef struct VpsPlannerColumn {
    uint32_t type_oid;
    uint32_t capabilities;
} VpsPlannerColumn;

typedef struct VpsPlannerConstraintInput {
    int32_t column;
    VpsPlanOperator operation;
    VpsPlanValueClass value_class;
    uint16_t source_index;
    int usable;
    int is_in;
    int null_safe;
} VpsPlannerConstraintInput;

typedef struct VpsPlannerOrderInput {
    uint16_t column;
    int descending;
} VpsPlannerOrderInput;

typedef struct VpsPlannerRequest {
    uint64_t source_fingerprint;
    const VpsPlannerColumn *columns;
    size_t column_count;
    uint64_t columns_used;
    const VpsPlannerConstraintInput *constraints;
    size_t constraint_count;
    const VpsPlannerOrderInput *order_terms;
    size_t order_count;
    const uint16_t *key_columns;
    size_t key_column_count;
    uint64_t estimated_base_rows;
    uint64_t relation_pages;
    size_t query_index_prefix;
    VpsLogger *logger;
} VpsPlannerRequest;

typedef struct VpsPlanConstraint {
    int32_t column;
    uint32_t type_oid;
    uint16_t source_index;
    uint16_t argument_index;
    uint16_t flags;
    uint8_t operation;
    uint8_t value_class;
} VpsPlanConstraint;

typedef struct VpsPlanOrderTerm {
    uint16_t column;
    uint8_t descending;
    uint8_t nulls_first;
} VpsPlanOrderTerm;

typedef struct VpsCompiledPlan {
    uint32_t version;
    uint32_t flags;
    uint64_t source_fingerprint;
    uint64_t estimated_rows;
    uint64_t estimated_cost_milli;
    uint16_t projection[VPS_PLAN_MAX_COLUMNS];
    uint16_t projection_count;
    VpsPlanConstraint constraints[VPS_PLAN_MAX_CONSTRAINTS];
    uint16_t constraint_count;
    VpsPlanOrderTerm order_terms[VPS_PLAN_MAX_ORDER_TERMS];
    uint16_t order_count;
    uint16_t argument_count;
    uint16_t selected_index_prefix;
} VpsCompiledPlan;

VpsPlannerResult vps_planner_compile(const VpsPlannerRequest *request,
                                     VpsCompiledPlan *plan);
VpsPlannerResult vps_plan_encode(const VpsCompiledPlan *plan,
                                 const VpsAllocator *allocator,
                                 VpsBuffer *encoded);
VpsPlannerResult vps_plan_decode(const char *encoded,
                                 size_t encoded_length,
                                 uint64_t expected_fingerprint,
                                 VpsCompiledPlan *plan);
const char *vps_planner_result_name(VpsPlannerResult result);

#endif
