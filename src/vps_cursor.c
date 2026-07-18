#include "vps_cursor.h"

#include <stddef.h>
#include <string.h>

static void vps_cursor_log(VpsCursorMachine *machine,
                           VpsCursorState old_state,
                           VpsCursorEvent event,
                           VpsCursorState new_state,
                           const char *outcome)
{
    VpsLogEvent log_event;
    const char *old_name = vps_cursor_state_name(old_state);
    const char *event_name = vps_cursor_event_name(event);
    const char *new_name = vps_cursor_state_name(new_state);
    if (machine == NULL || machine->logger == NULL || outcome == NULL ||
        vps_log_event_init(&log_event,
                           strcmp(outcome, "ok") == 0
                               ? VPS_LOG_LEVEL_DEBUG
                               : VPS_LOG_LEVEL_ERROR) != VPS_LOG_OK ||
        vps_log_event_add_string(&log_event, VPS_LOG_FIELD_OPERATION,
                                 "cursor", sizeof("cursor") - 1U) !=
            VPS_LOG_OK ||
        vps_log_event_add_string(&log_event, VPS_LOG_FIELD_PHASE, event_name,
                                 strlen(event_name)) != VPS_LOG_OK ||
        vps_log_event_add_string(&log_event, VPS_LOG_FIELD_ARGUMENT, old_name,
                                 strlen(old_name)) != VPS_LOG_OK ||
        vps_log_event_add_string(&log_event, VPS_LOG_FIELD_STATUS, new_name,
                                 strlen(new_name)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&log_event, VPS_LOG_FIELD_GENERATION,
                                 machine->cursor_id) != VPS_LOG_OK) {
        return;
    }
    vps_logger_emit(machine->logger, &log_event);
}

VpsCursorResult vps_cursor_machine_init(VpsCursorMachine *machine,
                                        uint64_t cursor_id,
                                        VpsLogger *logger)
{
    if (machine == NULL || cursor_id == 0U) return VPS_CURSOR_INVALID_ARGUMENT;
    (void)memset(machine, 0, sizeof(*machine));
    machine->state = VPS_CURSOR_NEW;
    machine->waiting_from = VPS_CURSOR_NEW;
    machine->cursor_id = cursor_id;
    machine->logger = logger;
    return VPS_CURSOR_OK;
}

static int vps_cursor_next_state(const VpsCursorMachine *machine,
                                 VpsCursorEvent event,
                                 VpsCursorState *next)
{
    VpsCursorState state = machine->state;
    if (event == VPS_CURSOR_EVENT_FAIL && state != VPS_CURSOR_CLOSED) {
        *next = VPS_CURSOR_FAILED;
        return 1;
    }
    if (event == VPS_CURSOR_EVENT_CLOSE && state != VPS_CURSOR_CLOSED) {
        *next = VPS_CURSOR_CLOSED;
        return 1;
    }
    switch (state) {
        case VPS_CURSOR_NEW:
            if (event == VPS_CURSOR_EVENT_OPEN) *next = VPS_CURSOR_OPEN;
            else return 0;
            return 1;
        case VPS_CURSOR_OPEN:
        case VPS_CURSOR_EOF:
            if (event == VPS_CURSOR_EVENT_FILTER) *next = VPS_CURSOR_FILTERING;
            else return 0;
            return 1;
        case VPS_CURSOR_FILTERING:
            if (event == VPS_CURSOR_EVENT_WAIT) *next = VPS_CURSOR_WAITING;
            else if (event == VPS_CURSOR_EVENT_ROW) *next = VPS_CURSOR_ROW_READY;
            else if (event == VPS_CURSOR_EVENT_COMPLETE) *next = VPS_CURSOR_EOF;
            else if (event == VPS_CURSOR_EVENT_CANCEL) *next = VPS_CURSOR_CANCELLING;
            else return 0;
            return 1;
        case VPS_CURSOR_WAITING:
            if (event == VPS_CURSOR_EVENT_RESUME) *next = machine->waiting_from;
            else if (event == VPS_CURSOR_EVENT_CANCEL) *next = VPS_CURSOR_CANCELLING;
            else return 0;
            return 1;
        case VPS_CURSOR_ROW_READY:
            if (event == VPS_CURSOR_EVENT_FETCH) *next = VPS_CURSOR_FILTERING;
            else if (event == VPS_CURSOR_EVENT_CANCEL) *next = VPS_CURSOR_CANCELLING;
            else return 0;
            return 1;
        case VPS_CURSOR_CANCELLING:
            if (event == VPS_CURSOR_EVENT_WAIT) *next = VPS_CURSOR_WAITING;
            else if (event == VPS_CURSOR_EVENT_COMPLETE) *next = VPS_CURSOR_EOF;
            else return 0;
            return 1;
        case VPS_CURSOR_FAILED:
        case VPS_CURSOR_CLOSED:
        default:
            return 0;
    }
}

VpsCursorResult vps_cursor_transition(VpsCursorMachine *machine,
                                      VpsCursorEvent event)
{
    VpsCursorState old_state;
    VpsCursorState next;
    if (machine == NULL || event < VPS_CURSOR_EVENT_OPEN ||
        event > VPS_CURSOR_EVENT_CLOSE) return VPS_CURSOR_INVALID_ARGUMENT;
    old_state = machine->state;
    if (!vps_cursor_next_state(machine, event, &next)) {
        vps_cursor_log(machine, old_state, event, old_state,
                       "invalid_transition");
        return VPS_CURSOR_INVALID_TRANSITION;
    }
    if (event == VPS_CURSOR_EVENT_WAIT) machine->waiting_from = old_state;
    machine->state = next;
    machine->transition_count += 1U;
    vps_cursor_log(machine, old_state, event, next, "ok");
    return VPS_CURSOR_OK;
}

const char *vps_cursor_state_name(VpsCursorState state)
{
    switch (state) {
        case VPS_CURSOR_NEW: return "new";
        case VPS_CURSOR_OPEN: return "open";
        case VPS_CURSOR_FILTERING: return "filtering";
        case VPS_CURSOR_WAITING: return "waiting";
        case VPS_CURSOR_ROW_READY: return "row_ready";
        case VPS_CURSOR_EOF: return "eof";
        case VPS_CURSOR_CANCELLING: return "cancelling";
        case VPS_CURSOR_FAILED: return "failed";
        case VPS_CURSOR_CLOSED: return "closed";
        default: return "invalid";
    }
}

const char *vps_cursor_event_name(VpsCursorEvent event)
{
    switch (event) {
        case VPS_CURSOR_EVENT_OPEN: return "open";
        case VPS_CURSOR_EVENT_FILTER: return "filter";
        case VPS_CURSOR_EVENT_FETCH: return "fetch";
        case VPS_CURSOR_EVENT_WAIT: return "wait";
        case VPS_CURSOR_EVENT_RESUME: return "resume";
        case VPS_CURSOR_EVENT_ROW: return "row";
        case VPS_CURSOR_EVENT_COMPLETE: return "complete";
        case VPS_CURSOR_EVENT_CANCEL: return "cancel";
        case VPS_CURSOR_EVENT_FAIL: return "fail";
        case VPS_CURSOR_EVENT_CLOSE: return "close";
        default: return "invalid";
    }
}

void vps_cursor_limits_default(VpsCursorLimits *limits)
{
    if (limits == NULL) return;
    limits->max_result_rows = VPS_CURSOR_DEFAULT_MAX_RESULT_ROWS;
    limits->max_result_bytes = sizeof(void *) == 4U
                                   ? VPS_CURSOR_X86_MAX_RESULT_BYTES
                                   : VPS_CURSOR_X64_MAX_RESULT_BYTES;
    limits->max_column_bytes = VPS_CURSOR_DEFAULT_MAX_COLUMN_BYTES;
    limits->max_query_bytes = VPS_CURSOR_DEFAULT_MAX_QUERY_BYTES;
    limits->max_identity_bytes = VPS_CURSOR_DEFAULT_MAX_IDENTITY_BYTES;
    limits->max_parameter_bytes = VPS_CURSOR_DEFAULT_MAX_PARAMETER_BYTES;
    limits->max_in_values = VPS_CURSOR_DEFAULT_MAX_IN_VALUES;
    limits->max_spatial_points = sizeof(void *) == 4U
                                     ? VPS_CURSOR_X86_MAX_SPATIAL_POINTS
                                     : VPS_CURSOR_X64_MAX_SPATIAL_POINTS;
    limits->max_spatial_depth = VPS_CURSOR_DEFAULT_MAX_SPATIAL_DEPTH;
}

VpsCursorLimitResult vps_cursor_budget_init(VpsCursorBudget *budget,
                                            const VpsCursorLimits *limits,
                                            uint64_t cursor_id,
                                            VpsLogger *logger)
{
    if (budget == NULL || limits == NULL || cursor_id == 0U ||
        limits->max_result_rows == 0U || limits->max_result_bytes == 0U ||
        limits->max_column_bytes == 0U || limits->max_query_bytes == 0U ||
        limits->max_identity_bytes == 0U ||
        limits->max_parameter_bytes == 0U || limits->max_in_values == 0U ||
        limits->max_spatial_points == 0U ||
        limits->max_spatial_depth == 0U)
        return VPS_CURSOR_LIMIT_INVALID_ARGUMENT;
    (void)memset(budget, 0, sizeof(*budget));
    budget->limits = *limits;
    budget->cursor_id = cursor_id;
    budget->logger = logger;
    return VPS_CURSOR_LIMIT_OK;
}

void vps_cursor_budget_reset(VpsCursorBudget *budget)
{
    if (budget == NULL) return;
    budget->result_rows = 0U;
    budget->result_bytes = 0U;
}

static void vps_cursor_limit_log(const VpsCursorBudget *budget,
                                 const char *name,
                                 uint64_t observed)
{
    VpsLogEvent event;
    if (budget == NULL || budget->logger == NULL || name == NULL ||
        vps_log_event_init(&event, VPS_LOG_LEVEL_WARN) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, "cursor",
                                 sizeof("cursor") - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, "limit",
                                 sizeof("limit") - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_ARGUMENT, name,
                                 strlen(name)) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, "exceeded",
                                 sizeof("exceeded") - 1U) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_BYTE_COUNT,
                                 observed) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_GENERATION,
                                 budget->cursor_id) != VPS_LOG_OK)
        return;
    vps_logger_emit(budget->logger, &event);
}

VpsCursorLimitResult vps_cursor_budget_check_query(
    const VpsCursorBudget *budget,
    uint64_t query_bytes,
    uint64_t parameter_bytes,
    uint64_t in_values)
{
    if (budget == NULL || budget->cursor_id == 0U)
        return VPS_CURSOR_LIMIT_INVALID_ARGUMENT;
    if (query_bytes > budget->limits.max_query_bytes ||
        parameter_bytes > budget->limits.max_parameter_bytes ||
        in_values > budget->limits.max_in_values)
        return VPS_CURSOR_LIMIT_EXCEEDED;
    return VPS_CURSOR_LIMIT_OK;
}

VpsCursorLimitResult vps_cursor_budget_observe_row(VpsCursorBudget *budget,
                                                   uint64_t row_bytes,
                                                   uint64_t largest_column,
                                                   uint64_t identity_bytes)
{
    uint64_t next_rows;
    uint64_t next_bytes;
    uint64_t observed;
    const char *limit_name = NULL;
    if (budget == NULL || budget->cursor_id == 0U)
        return VPS_CURSOR_LIMIT_INVALID_ARGUMENT;
    if (budget->result_rows == UINT64_MAX ||
        row_bytes > UINT64_MAX - budget->result_bytes)
        return VPS_CURSOR_LIMIT_OVERFLOW;
    next_rows = budget->result_rows + 1U;
    next_bytes = budget->result_bytes + row_bytes;
    if (largest_column > budget->limits.max_column_bytes)
        limit_name = "max_column_bytes";
    else if (identity_bytes > budget->limits.max_identity_bytes)
        limit_name = "max_identity_bytes";
    else if (next_rows > budget->limits.max_result_rows)
        limit_name = "max_result_rows";
    else if (next_bytes > budget->limits.max_result_bytes)
        limit_name = "max_result_bytes";
    if (limit_name != NULL) {
        observed = strcmp(limit_name, "max_column_bytes") == 0
                       ? largest_column
                       : strcmp(limit_name, "max_identity_bytes") == 0
                             ? identity_bytes
                             : strcmp(limit_name, "max_result_rows") == 0
                                   ? next_rows
                                   : next_bytes;
        vps_cursor_limit_log(budget, limit_name, observed);
        return VPS_CURSOR_LIMIT_EXCEEDED;
    }
    budget->result_rows = next_rows;
    budget->result_bytes = next_bytes;
    return VPS_CURSOR_LIMIT_OK;
}
