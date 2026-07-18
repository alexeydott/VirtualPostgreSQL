#ifndef VPS_QUERY_BOUNDARY_H
#define VPS_QUERY_BOUNDARY_H

#include "vps_error.h"
#include "vps_logging.h"

#include <stddef.h>
#include <stdint.h>

#define VPS_QUERY_BOUNDARY_FORMAT_VERSION UINT32_C(1)
#define VPS_QUERY_BOUNDARY_MAX_SEARCH_PATH_BYTES 1024U
#define VPS_QUERY_BOUNDARY_BEGIN_SQL "BEGIN READ ONLY"
#define VPS_QUERY_BOUNDARY_SET_CONFIG_SQL \
    "SELECT pg_catalog.set_config($1, $2, true)"
#define VPS_QUERY_BOUNDARY_COMMIT_SQL "COMMIT"
#define VPS_QUERY_BOUNDARY_ROLLBACK_SQL "ROLLBACK"

typedef enum VpsQueryBoundaryResult {
    VPS_QUERY_BOUNDARY_OK = 0,
    VPS_QUERY_BOUNDARY_INVALID_ARGUMENT = 1,
    VPS_QUERY_BOUNDARY_INVALID_STATE = 2,
    VPS_QUERY_BOUNDARY_CLIENT_ERROR = 3,
    VPS_QUERY_BOUNDARY_ROW_LIMIT = 4,
    VPS_QUERY_BOUNDARY_BYTE_LIMIT = 5,
    VPS_QUERY_BOUNDARY_DEADLINE = 6,
    VPS_QUERY_BOUNDARY_ROLLBACK_ERROR = 7
} VpsQueryBoundaryResult;

typedef enum VpsQueryBoundaryState {
    VPS_QUERY_BOUNDARY_NEW = 0,
    VPS_QUERY_BOUNDARY_STARTING = 1,
    VPS_QUERY_BOUNDARY_ACTIVE = 2,
    VPS_QUERY_BOUNDARY_COMMITTING = 3,
    VPS_QUERY_BOUNDARY_COMMITTED = 4,
    VPS_QUERY_BOUNDARY_ROLLING_BACK = 5,
    VPS_QUERY_BOUNDARY_ROLLED_BACK = 6,
    VPS_QUERY_BOUNDARY_FAILED = 7
} VpsQueryBoundaryState;

typedef struct VpsQueryBoundaryPolicy {
    const char *search_path;
    size_t search_path_length;
    uint64_t statement_timeout_ms;
    uint64_t lock_timeout_ms;
    uint64_t max_rows;
    uint64_t max_bytes;
    uint64_t deadline_ms;
} VpsQueryBoundaryPolicy;

typedef VpsQueryBoundaryResult (*VpsQueryBoundaryBeginFunction)(
    void *context, VpsError *error);
typedef VpsQueryBoundaryResult (*VpsQueryBoundaryConfigureFunction)(
    void *context, const VpsQueryBoundaryPolicy *policy, VpsError *error);
typedef VpsQueryBoundaryResult (*VpsQueryBoundaryEndFunction)(
    void *context, VpsError *error);

typedef struct VpsQueryBoundaryExecutor {
    uint32_t structure_size;
    uint32_t format_version;
    void *context;
    VpsQueryBoundaryBeginFunction begin_read_only;
    VpsQueryBoundaryConfigureFunction configure_local;
    VpsQueryBoundaryEndFunction commit;
    VpsQueryBoundaryEndFunction rollback;
} VpsQueryBoundaryExecutor;

/* Boundary borrows executor context, search_path and logger until terminal. */
typedef struct VpsQueryBoundary {
    VpsQueryBoundaryExecutor executor;
    VpsQueryBoundaryPolicy policy;
    VpsLogger *logger;
    VpsQueryBoundaryState state;
    uint64_t row_count;
    uint64_t byte_count;
    uint64_t started_ms;
    int initialized;
} VpsQueryBoundary;

VpsQueryBoundaryResult vps_query_boundary_init(
    VpsQueryBoundary *boundary,
    const VpsQueryBoundaryExecutor *executor,
    const VpsQueryBoundaryPolicy *policy,
    VpsLogger *logger);
VpsQueryBoundaryResult vps_query_boundary_open(
    VpsQueryBoundary *boundary,
    uint64_t now_ms,
    VpsError *error);
VpsQueryBoundaryResult vps_query_boundary_observe(
    VpsQueryBoundary *boundary,
    uint64_t row_bytes,
    uint64_t now_ms,
    VpsError *error);
VpsQueryBoundaryResult vps_query_boundary_finish(
    VpsQueryBoundary *boundary,
    VpsError *error);
VpsQueryBoundaryResult vps_query_boundary_fail(
    VpsQueryBoundary *boundary,
    VpsError *error);
VpsQueryBoundaryResult vps_query_boundary_cleanup(
    VpsQueryBoundary *boundary,
    VpsError *error);

const char *vps_query_boundary_result_name(VpsQueryBoundaryResult result);
const char *vps_query_boundary_state_name(VpsQueryBoundaryState state);

#endif
