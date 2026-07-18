#ifndef VPS_DML_H
#define VPS_DML_H

#include "vps_row_identity.h"
#include "vps_table_metadata.h"

#define VPS_DML_MAX_COLUMNS 1024U
#define VPS_DML_MAX_PARAMETERS (VPS_DML_MAX_COLUMNS + VPS_METADATA_MAX_KEY_COLUMNS + 1U)
#define VPS_DML_QUERY_LIMIT (1024U * 1024U)

typedef enum VpsDmlOperation {
    VPS_DML_INSERT = 1,
    VPS_DML_UPDATE = 2,
    VPS_DML_DELETE = 3
} VpsDmlOperation;

typedef enum VpsDmlOptimisticMode {
    VPS_DML_OPTIMISTIC_OFF = 0,
    VPS_DML_OPTIMISTIC_COLUMN = 1,
    VPS_DML_OPTIMISTIC_XMIN = 2
} VpsDmlOptimisticMode;

typedef enum VpsDmlResult {
    VPS_DML_OK = 0,
    VPS_DML_INVALID_ARGUMENT = 1,
    VPS_DML_READ_ONLY = 2,
    VPS_DML_NO_KEY = 3,
    VPS_DML_UNSUPPORTED_RELATION = 4,
    VPS_DML_UNSUPPORTED_TYPE = 5,
    VPS_DML_GENERATED_COLUMN = 6,
    VPS_DML_IDENTITY_ALWAYS = 7,
    VPS_DML_OPTIMISTIC_INVALID = 8,
    VPS_DML_MALFORMED_IDENTITY = 9,
    VPS_DML_LIMIT = 10,
    VPS_DML_OUT_OF_MEMORY = 11,
    VPS_DML_NOT_FOUND = 12,
    VPS_DML_CONFLICT = 13,
    VPS_DML_INVARIANT = 14
} VpsDmlResult;

typedef struct VpsDmlPolicy {
    const VpsTableMetadata *metadata;
    uint16_t visible_to_metadata[VPS_DML_MAX_COLUMNS];
    uint16_t key_visible[VPS_METADATA_MAX_KEY_COLUMNS];
    VpsTypeSelection selections[VPS_DML_MAX_COLUMNS];
    unsigned char insertable[VPS_DML_MAX_COLUMNS];
    unsigned char updatable[VPS_DML_MAX_COLUMNS];
    size_t visible_count;
    size_t key_count;
    size_t version_visible;
    VpsDmlOptimisticMode optimistic_mode;
    int writable;
} VpsDmlPolicy;

typedef enum VpsDmlParameterSource {
    VPS_DML_PARAMETER_NEW_VALUE = 1,
    VPS_DML_PARAMETER_OLD_KEY = 2,
    VPS_DML_PARAMETER_OLD_OPTIMISTIC = 3
} VpsDmlParameterSource;

typedef struct VpsDmlParameterSlot {
    VpsDmlParameterSource source;
    uint16_t index;
    uint32_t type_oid;
} VpsDmlParameterSlot;

typedef struct VpsDmlPlan {
    VpsBuffer query;
    VpsDmlParameterSlot parameters[VPS_DML_MAX_PARAMETERS];
    uint16_t returning_visible[VPS_METADATA_MAX_KEY_COLUMNS + 1U];
    size_t parameter_count;
    size_t returning_count;
    size_t key_returning_count;
    VpsDmlOperation operation;
    int returns_optimistic;
    int initialized;
} VpsDmlPlan;

VpsDmlResult vps_dml_policy_build(
    const VpsTableMetadata *metadata,
    int mode_rw,
    VpsDmlOptimisticMode optimistic_mode,
    const char *version_column,
    size_t version_column_length,
    VpsDmlPolicy *policy);
VpsDmlResult vps_dml_plan_build(
    const VpsAllocator *allocator,
    const VpsDmlPolicy *policy,
    VpsDmlOperation operation,
    const unsigned char *included_columns,
    size_t included_count,
    const VpsRowIdentityView *original_identity,
    VpsDmlPlan *plan);
void vps_dml_plan_reset(VpsDmlPlan *plan);
VpsDmlResult vps_dml_classify_count(VpsDmlOperation operation,
                                    VpsDmlOptimisticMode optimistic_mode,
                                    uint64_t affected_count);
const char *vps_dml_result_name(VpsDmlResult result);
const char *vps_dml_operation_name(VpsDmlOperation operation);

#endif
