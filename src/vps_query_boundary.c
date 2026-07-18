#include "vps_query_boundary.h"

#include <string.h>

static int vps_query_boundary_policy_valid(
    const VpsQueryBoundaryPolicy *policy)
{
    size_t index;
    if (policy == NULL || policy->search_path == NULL ||
        policy->search_path_length == 0U ||
        policy->search_path_length > VPS_QUERY_BOUNDARY_MAX_SEARCH_PATH_BYTES ||
        policy->statement_timeout_ms == 0U || policy->lock_timeout_ms == 0U ||
        policy->max_rows == 0U || policy->max_bytes == 0U ||
        policy->deadline_ms == 0U) return 0;
    for (index = 0U; index < policy->search_path_length; ++index) {
        unsigned char value = (unsigned char)policy->search_path[index];
        if (value == 0U || value < 0x20U || value == 0x7fU) return 0;
    }
    return 1;
}

static void vps_query_boundary_log(VpsQueryBoundary *boundary,
                                   VpsQueryBoundaryResult result,
                                   VpsLogLevel level)
{
    VpsLogEvent event;
    const char *state = vps_query_boundary_state_name(boundary->state);
    const char *status = vps_query_boundary_result_name(result);
    if (boundary->logger == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK) return;
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION,
                                   "query_boundary", 14U);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE,
                                   state, strlen(state));
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS,
                                   status, strlen(status));
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_ROW_COUNT,
                                   boundary->row_count);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_BYTE_COUNT,
                                   boundary->byte_count);
    vps_logger_emit(boundary->logger, &event);
}

VpsQueryBoundaryResult vps_query_boundary_init(
    VpsQueryBoundary *boundary,
    const VpsQueryBoundaryExecutor *executor,
    const VpsQueryBoundaryPolicy *policy,
    VpsLogger *logger)
{
    if (boundary == NULL || executor == NULL ||
        executor->structure_size < sizeof(*executor) ||
        executor->format_version != VPS_QUERY_BOUNDARY_FORMAT_VERSION ||
        executor->begin_read_only == NULL ||
        executor->configure_local == NULL || executor->commit == NULL ||
        executor->rollback == NULL || !vps_query_boundary_policy_valid(policy)) {
        return VPS_QUERY_BOUNDARY_INVALID_ARGUMENT;
    }
    (void)memset(boundary, 0, sizeof(*boundary));
    boundary->executor = *executor;
    boundary->policy = *policy;
    boundary->logger = logger;
    boundary->state = VPS_QUERY_BOUNDARY_NEW;
    boundary->initialized = 1;
    return VPS_QUERY_BOUNDARY_OK;
}

static VpsQueryBoundaryResult vps_query_boundary_rollback(
    VpsQueryBoundary *boundary,
    VpsError *error,
    VpsQueryBoundaryResult cause)
{
    VpsQueryBoundaryResult rollback_result;
    boundary->state = VPS_QUERY_BOUNDARY_ROLLING_BACK;
    rollback_result = boundary->executor.rollback(boundary->executor.context,
                                                  error);
    if (rollback_result != VPS_QUERY_BOUNDARY_OK) {
        boundary->state = VPS_QUERY_BOUNDARY_FAILED;
        vps_query_boundary_log(boundary, VPS_QUERY_BOUNDARY_ROLLBACK_ERROR,
                               VPS_LOG_LEVEL_ERROR);
        return VPS_QUERY_BOUNDARY_ROLLBACK_ERROR;
    }
    boundary->state = VPS_QUERY_BOUNDARY_ROLLED_BACK;
    vps_query_boundary_log(boundary, cause,
                           cause == VPS_QUERY_BOUNDARY_OK
                               ? VPS_LOG_LEVEL_INFO : VPS_LOG_LEVEL_WARN);
    return cause;
}

VpsQueryBoundaryResult vps_query_boundary_open(
    VpsQueryBoundary *boundary,
    uint64_t now_ms,
    VpsError *error)
{
    VpsQueryBoundaryResult result;
    if (boundary == NULL || !boundary->initialized ||
        boundary->state != VPS_QUERY_BOUNDARY_NEW) {
        return VPS_QUERY_BOUNDARY_INVALID_STATE;
    }
    boundary->state = VPS_QUERY_BOUNDARY_STARTING;
    result = boundary->executor.begin_read_only(boundary->executor.context,
                                                error);
    if (result != VPS_QUERY_BOUNDARY_OK) {
        boundary->state = VPS_QUERY_BOUNDARY_FAILED;
        vps_query_boundary_log(boundary, VPS_QUERY_BOUNDARY_CLIENT_ERROR,
                               VPS_LOG_LEVEL_ERROR);
        return VPS_QUERY_BOUNDARY_CLIENT_ERROR;
    }
    result = boundary->executor.configure_local(boundary->executor.context,
                                                &boundary->policy, error);
    if (result != VPS_QUERY_BOUNDARY_OK) {
        return vps_query_boundary_rollback(
            boundary, error, VPS_QUERY_BOUNDARY_CLIENT_ERROR);
    }
    boundary->started_ms = now_ms;
    boundary->state = VPS_QUERY_BOUNDARY_ACTIVE;
    vps_query_boundary_log(boundary, VPS_QUERY_BOUNDARY_OK,
                           VPS_LOG_LEVEL_INFO);
    return VPS_QUERY_BOUNDARY_OK;
}

VpsQueryBoundaryResult vps_query_boundary_observe(
    VpsQueryBoundary *boundary,
    uint64_t row_bytes,
    uint64_t now_ms,
    VpsError *error)
{
    VpsQueryBoundaryResult result = VPS_QUERY_BOUNDARY_OK;
    if (boundary == NULL || !boundary->initialized ||
        boundary->state != VPS_QUERY_BOUNDARY_ACTIVE) {
        return VPS_QUERY_BOUNDARY_INVALID_STATE;
    }
    if (boundary->row_count == UINT64_MAX ||
        boundary->byte_count > UINT64_MAX - row_bytes) {
        result = VPS_QUERY_BOUNDARY_BYTE_LIMIT;
    } else {
        ++boundary->row_count;
        boundary->byte_count += row_bytes;
        if (boundary->row_count > boundary->policy.max_rows) {
            result = VPS_QUERY_BOUNDARY_ROW_LIMIT;
        } else if (boundary->byte_count > boundary->policy.max_bytes) {
            result = VPS_QUERY_BOUNDARY_BYTE_LIMIT;
        } else if (now_ms < boundary->started_ms ||
                   now_ms - boundary->started_ms >
                       boundary->policy.deadline_ms) {
            result = VPS_QUERY_BOUNDARY_DEADLINE;
        }
    }
    if (result != VPS_QUERY_BOUNDARY_OK) {
        return vps_query_boundary_rollback(boundary, error, result);
    }
    return VPS_QUERY_BOUNDARY_OK;
}

VpsQueryBoundaryResult vps_query_boundary_finish(
    VpsQueryBoundary *boundary,
    VpsError *error)
{
    VpsQueryBoundaryResult result;
    if (boundary == NULL || !boundary->initialized ||
        boundary->state != VPS_QUERY_BOUNDARY_ACTIVE) {
        return VPS_QUERY_BOUNDARY_INVALID_STATE;
    }
    boundary->state = VPS_QUERY_BOUNDARY_COMMITTING;
    result = boundary->executor.commit(boundary->executor.context, error);
    if (result != VPS_QUERY_BOUNDARY_OK) {
        boundary->state = VPS_QUERY_BOUNDARY_FAILED;
        vps_query_boundary_log(boundary, VPS_QUERY_BOUNDARY_CLIENT_ERROR,
                               VPS_LOG_LEVEL_ERROR);
        return VPS_QUERY_BOUNDARY_CLIENT_ERROR;
    }
    boundary->state = VPS_QUERY_BOUNDARY_COMMITTED;
    vps_query_boundary_log(boundary, VPS_QUERY_BOUNDARY_OK,
                           VPS_LOG_LEVEL_INFO);
    return VPS_QUERY_BOUNDARY_OK;
}

VpsQueryBoundaryResult vps_query_boundary_fail(
    VpsQueryBoundary *boundary,
    VpsError *error)
{
    if (boundary == NULL || !boundary->initialized ||
        boundary->state != VPS_QUERY_BOUNDARY_ACTIVE) {
        return VPS_QUERY_BOUNDARY_INVALID_STATE;
    }
    return vps_query_boundary_rollback(boundary, error,
                                       VPS_QUERY_BOUNDARY_CLIENT_ERROR);
}

VpsQueryBoundaryResult vps_query_boundary_cleanup(
    VpsQueryBoundary *boundary,
    VpsError *error)
{
    VpsQueryBoundaryResult result = VPS_QUERY_BOUNDARY_OK;
    if (boundary == NULL) return VPS_QUERY_BOUNDARY_INVALID_ARGUMENT;
    if (!boundary->initialized) return VPS_QUERY_BOUNDARY_OK;
    if (boundary->state == VPS_QUERY_BOUNDARY_ACTIVE) {
        result = vps_query_boundary_rollback(boundary, error,
                                             VPS_QUERY_BOUNDARY_OK);
    }
    if (boundary->state == VPS_QUERY_BOUNDARY_STARTING ||
        boundary->state == VPS_QUERY_BOUNDARY_COMMITTING ||
        boundary->state == VPS_QUERY_BOUNDARY_ROLLING_BACK) {
        return VPS_QUERY_BOUNDARY_INVALID_STATE;
    }
    boundary->initialized = 0;
    return result;
}

const char *vps_query_boundary_result_name(VpsQueryBoundaryResult result)
{
    static const char *const names[] = {
        "ok", "invalid_argument", "invalid_state", "client_error",
        "row_limit", "byte_limit", "deadline", "rollback_error"
    };
    if (result < VPS_QUERY_BOUNDARY_OK ||
        result > VPS_QUERY_BOUNDARY_ROLLBACK_ERROR) return "unknown";
    return names[(size_t)result];
}

const char *vps_query_boundary_state_name(VpsQueryBoundaryState state)
{
    static const char *const names[] = {
        "new", "starting", "active", "committing", "committed",
        "rolling_back", "rolled_back", "failed"
    };
    if (state < VPS_QUERY_BOUNDARY_NEW || state > VPS_QUERY_BOUNDARY_FAILED) {
        return "unknown";
    }
    return names[(size_t)state];
}
