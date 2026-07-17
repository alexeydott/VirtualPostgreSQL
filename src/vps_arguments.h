#ifndef VPS_ARGUMENTS_H
#define VPS_ARGUMENTS_H

#include "vps_logging.h"
#include "vps_memory.h"
#include "vps_platform.h"
#include "vps_secure_memory.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_ARGUMENT_MAX_COUNT 64U
#define VPS_ARGUMENT_NAME_LIMIT 64U
#define VPS_ARGUMENT_VALUE_LIMIT 16384U
#define VPS_ARGUMENT_TOTAL_LIMIT 65536U

typedef uint64_t VpsArgumentMask;

typedef enum VpsArgumentId {
    VPS_ARGUMENT_ID_CREDENTIAL_REF = 0,
    VPS_ARGUMENT_ID_SERVICE,
    VPS_ARGUMENT_ID_SERVICE_FILE,
    VPS_ARGUMENT_ID_PROFILE,
    VPS_ARGUMENT_ID_CONNSTR,
    VPS_ARGUMENT_ID_SOURCE,
    VPS_ARGUMENT_ID_SCHEMA,
    VPS_ARGUMENT_ID_TABLE,
    VPS_ARGUMENT_ID_QUERY,
    VPS_ARGUMENT_ID_QUERY_PROFILE,
    VPS_ARGUMENT_ID_MODE,
    VPS_ARGUMENT_ID_ALLOW_VIEW,
    VPS_ARGUMENT_ID_ALLOW_MATERIALIZED_VIEW,
    VPS_ARGUMENT_ID_ALLOW_FOREIGN_TABLE,
    VPS_ARGUMENT_ID_KEY_COLUMNS,
    VPS_ARGUMENT_ID_OPTIMISTIC_LOCK,
    VPS_ARGUMENT_ID_VERSION_COLUMN,
    VPS_ARGUMENT_ID_GEOMETRY,
    VPS_ARGUMENT_ID_SRID,
    VPS_ARGUMENT_ID_CONNECT_TIMEOUT,
    VPS_ARGUMENT_ID_STATEMENT_TIMEOUT,
    VPS_ARGUMENT_ID_LOCK_TIMEOUT,
    VPS_ARGUMENT_ID_POOL_MIN,
    VPS_ARGUMENT_ID_POOL_MAX,
    VPS_ARGUMENT_ID_POOL_IDLE_TIMEOUT,
    VPS_ARGUMENT_ID_POOL_WAIT_TIMEOUT,
    VPS_ARGUMENT_ID_POOL_VALIDATION_INTERVAL,
    VPS_ARGUMENT_ID_POOL_RESET,
    VPS_ARGUMENT_ID_POOL_READONLY_SEPARATE,
    VPS_ARGUMENT_ID_METADATA_MODE,
    VPS_ARGUMENT_ID_COUNT,
    VPS_ARGUMENT_ID_UNKNOWN = 255
} VpsArgumentId;

#define VPS_ARGUMENT_CREDENTIAL_REF \
    (UINT64_C(1) << VPS_ARGUMENT_ID_CREDENTIAL_REF)
#define VPS_ARGUMENT_SERVICE \
    (UINT64_C(1) << VPS_ARGUMENT_ID_SERVICE)
#define VPS_ARGUMENT_SERVICE_FILE \
    (UINT64_C(1) << VPS_ARGUMENT_ID_SERVICE_FILE)
#define VPS_ARGUMENT_PROFILE \
    (UINT64_C(1) << VPS_ARGUMENT_ID_PROFILE)
#define VPS_ARGUMENT_CONNSTR \
    (UINT64_C(1) << VPS_ARGUMENT_ID_CONNSTR)
#define VPS_ARGUMENT_SOURCE \
    (UINT64_C(1) << VPS_ARGUMENT_ID_SOURCE)
#define VPS_ARGUMENT_SCHEMA \
    (UINT64_C(1) << VPS_ARGUMENT_ID_SCHEMA)
#define VPS_ARGUMENT_TABLE \
    (UINT64_C(1) << VPS_ARGUMENT_ID_TABLE)
#define VPS_ARGUMENT_QUERY \
    (UINT64_C(1) << VPS_ARGUMENT_ID_QUERY)
#define VPS_ARGUMENT_QUERY_PROFILE \
    (UINT64_C(1) << VPS_ARGUMENT_ID_QUERY_PROFILE)
#define VPS_ARGUMENT_MODE \
    (UINT64_C(1) << VPS_ARGUMENT_ID_MODE)
#define VPS_ARGUMENT_ALLOW_VIEW \
    (UINT64_C(1) << VPS_ARGUMENT_ID_ALLOW_VIEW)
#define VPS_ARGUMENT_ALLOW_MATERIALIZED_VIEW \
    (UINT64_C(1) << VPS_ARGUMENT_ID_ALLOW_MATERIALIZED_VIEW)
#define VPS_ARGUMENT_ALLOW_FOREIGN_TABLE \
    (UINT64_C(1) << VPS_ARGUMENT_ID_ALLOW_FOREIGN_TABLE)
#define VPS_ARGUMENT_KEY_COLUMNS \
    (UINT64_C(1) << VPS_ARGUMENT_ID_KEY_COLUMNS)
#define VPS_ARGUMENT_OPTIMISTIC_LOCK \
    (UINT64_C(1) << VPS_ARGUMENT_ID_OPTIMISTIC_LOCK)
#define VPS_ARGUMENT_VERSION_COLUMN \
    (UINT64_C(1) << VPS_ARGUMENT_ID_VERSION_COLUMN)
#define VPS_ARGUMENT_GEOMETRY \
    (UINT64_C(1) << VPS_ARGUMENT_ID_GEOMETRY)
#define VPS_ARGUMENT_SRID \
    (UINT64_C(1) << VPS_ARGUMENT_ID_SRID)
#define VPS_ARGUMENT_CONNECT_TIMEOUT \
    (UINT64_C(1) << VPS_ARGUMENT_ID_CONNECT_TIMEOUT)
#define VPS_ARGUMENT_STATEMENT_TIMEOUT \
    (UINT64_C(1) << VPS_ARGUMENT_ID_STATEMENT_TIMEOUT)
#define VPS_ARGUMENT_LOCK_TIMEOUT \
    (UINT64_C(1) << VPS_ARGUMENT_ID_LOCK_TIMEOUT)
#define VPS_ARGUMENT_POOL_MIN \
    (UINT64_C(1) << VPS_ARGUMENT_ID_POOL_MIN)
#define VPS_ARGUMENT_POOL_MAX \
    (UINT64_C(1) << VPS_ARGUMENT_ID_POOL_MAX)
#define VPS_ARGUMENT_POOL_IDLE_TIMEOUT \
    (UINT64_C(1) << VPS_ARGUMENT_ID_POOL_IDLE_TIMEOUT)
#define VPS_ARGUMENT_POOL_WAIT_TIMEOUT \
    (UINT64_C(1) << VPS_ARGUMENT_ID_POOL_WAIT_TIMEOUT)
#define VPS_ARGUMENT_POOL_VALIDATION_INTERVAL \
    (UINT64_C(1) << VPS_ARGUMENT_ID_POOL_VALIDATION_INTERVAL)
#define VPS_ARGUMENT_POOL_RESET \
    (UINT64_C(1) << VPS_ARGUMENT_ID_POOL_RESET)
#define VPS_ARGUMENT_POOL_READONLY_SEPARATE \
    (UINT64_C(1) << VPS_ARGUMENT_ID_POOL_READONLY_SEPARATE)
#define VPS_ARGUMENT_METADATA_MODE \
    (UINT64_C(1) << VPS_ARGUMENT_ID_METADATA_MODE)

#define VPS_ARGUMENT_CONNECTION_MODES \
    (VPS_ARGUMENT_CREDENTIAL_REF | VPS_ARGUMENT_SERVICE | \
     VPS_ARGUMENT_PROFILE | VPS_ARGUMENT_CONNSTR)

typedef enum VpsArgumentType {
    VPS_ARGUMENT_TYPE_STRING = 1,
    VPS_ARGUMENT_TYPE_UINT32 = 2,
    VPS_ARGUMENT_TYPE_BOOLEAN = 3,
    VPS_ARGUMENT_TYPE_ENUM = 4
} VpsArgumentType;

typedef enum VpsArgumentEnumValue {
    VPS_ARGUMENT_ENUM_NONE = 0,
    VPS_ARGUMENT_ENUM_SOURCE_TABLE,
    VPS_ARGUMENT_ENUM_SOURCE_QUERY,
    VPS_ARGUMENT_ENUM_MODE_RO,
    VPS_ARGUMENT_ENUM_MODE_RW,
    VPS_ARGUMENT_ENUM_GEOMETRY_WKT,
    VPS_ARGUMENT_ENUM_GEOMETRY_WKB,
    VPS_ARGUMENT_ENUM_GEOMETRY_EWKT,
    VPS_ARGUMENT_ENUM_GEOMETRY_EWKB,
    VPS_ARGUMENT_ENUM_POOL_DISCARD_ALL,
    VPS_ARGUMENT_ENUM_POOL_STRICT_RESET,
    VPS_ARGUMENT_ENUM_METADATA_LIVE,
    VPS_ARGUMENT_ENUM_METADATA_CACHED
} VpsArgumentEnumValue;

typedef enum VpsArgumentsResult {
    VPS_ARGUMENTS_OK = 0,
    VPS_ARGUMENTS_INVALID_ARGUMENT = 1,
    VPS_ARGUMENTS_UNKNOWN_ARGUMENT = 2,
    VPS_ARGUMENTS_DUPLICATE_ARGUMENT = 3,
    VPS_ARGUMENTS_MALFORMED = 4,
    VPS_ARGUMENTS_LIMIT_EXCEEDED = 5,
    VPS_ARGUMENTS_RANGE_ERROR = 6,
    VPS_ARGUMENTS_INCOMPATIBLE = 7,
    VPS_ARGUMENTS_OUT_OF_MEMORY = 8,
    VPS_ARGUMENTS_CLEANUP_FAILED = 9
} VpsArgumentsResult;

typedef struct VpsArgumentInput {
    const char *text;
    size_t length;
} VpsArgumentInput;

typedef struct VpsArgumentValue {
    VpsArgumentType type;
    size_t offset;
    size_t length;
    uint32_t uint32_value;
    VpsArgumentEnumValue enum_value;
    int boolean_value;
    int present;
} VpsArgumentValue;

typedef struct VpsArgumentsDiagnostic {
    VpsArgumentsResult result;
    VpsArgumentId argument_id;
    VpsArgumentMask presence;
    int sensitive;
} VpsArgumentsDiagnostic;

/*
 * Parsed arguments own a single secure allocation containing all textual
 * values. Values are immutable until the next successful parse or reset.
 * Parsing is transactional: an error leaves the previous result unchanged.
 * The allocator/platform/logger are borrowed through the initialized owner;
 * callers serialize mutation because this value is not thread-safe.
 */
typedef struct VpsParsedArguments {
    VpsSensitiveMemory storage;
    VpsArgumentValue values[VPS_ARGUMENT_ID_COUNT];
    VpsArgumentMask presence;
    int initialized;
} VpsParsedArguments;

VpsArgumentsResult vps_arguments_init(
    VpsParsedArguments *arguments,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger);
VpsArgumentsResult vps_arguments_parse(
    VpsParsedArguments *arguments,
    const VpsArgumentInput *inputs,
    size_t input_count,
    VpsArgumentsDiagnostic *diagnostic);
VpsArgumentsResult vps_arguments_reset(VpsParsedArguments *arguments);

const VpsArgumentValue *vps_arguments_get(const VpsParsedArguments *arguments,
                                          VpsArgumentId argument_id);
const char *vps_argument_text(const VpsParsedArguments *arguments,
                              VpsArgumentId argument_id,
                              size_t *length);
const char *vps_argument_name(VpsArgumentId argument_id);
int vps_argument_is_sensitive(VpsArgumentId argument_id);
const char *vps_arguments_result_name(VpsArgumentsResult result);

#endif
