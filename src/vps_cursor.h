#ifndef VPS_CURSOR_H
#define VPS_CURSOR_H

#include "vps_logging.h"

#include <stdint.h>

typedef enum VpsCursorState {
    VPS_CURSOR_NEW = 0,
    VPS_CURSOR_OPEN = 1,
    VPS_CURSOR_FILTERING = 2,
    VPS_CURSOR_WAITING = 3,
    VPS_CURSOR_ROW_READY = 4,
    VPS_CURSOR_EOF = 5,
    VPS_CURSOR_CANCELLING = 6,
    VPS_CURSOR_FAILED = 7,
    VPS_CURSOR_CLOSED = 8
} VpsCursorState;

typedef enum VpsCursorEvent {
    VPS_CURSOR_EVENT_OPEN = 0,
    VPS_CURSOR_EVENT_FILTER = 1,
    VPS_CURSOR_EVENT_FETCH = 2,
    VPS_CURSOR_EVENT_WAIT = 3,
    VPS_CURSOR_EVENT_RESUME = 4,
    VPS_CURSOR_EVENT_ROW = 5,
    VPS_CURSOR_EVENT_COMPLETE = 6,
    VPS_CURSOR_EVENT_CANCEL = 7,
    VPS_CURSOR_EVENT_FAIL = 8,
    VPS_CURSOR_EVENT_CLOSE = 9
} VpsCursorEvent;

typedef enum VpsCursorResult {
    VPS_CURSOR_OK = 0,
    VPS_CURSOR_INVALID_ARGUMENT = 1,
    VPS_CURSOR_INVALID_TRANSITION = 2
} VpsCursorResult;

#define VPS_CURSOR_DEFAULT_MAX_RESULT_ROWS UINT64_C(10000000)
#define VPS_CURSOR_X86_MAX_RESULT_BYTES UINT64_C(268435456)
#define VPS_CURSOR_X64_MAX_RESULT_BYTES UINT64_C(1073741824)
#define VPS_CURSOR_DEFAULT_MAX_COLUMN_BYTES UINT64_C(16777216)
#define VPS_CURSOR_DEFAULT_MAX_QUERY_BYTES UINT64_C(1048576)
#define VPS_CURSOR_DEFAULT_MAX_IDENTITY_BYTES UINT64_C(65536)
#define VPS_CURSOR_DEFAULT_MAX_PARAMETER_BYTES UINT64_C(4194304)
#define VPS_CURSOR_DEFAULT_MAX_IN_VALUES UINT64_C(4096)
#define VPS_CURSOR_X86_MAX_SPATIAL_POINTS UINT64_C(250000)
#define VPS_CURSOR_X64_MAX_SPATIAL_POINTS UINT64_C(1000000)
#define VPS_CURSOR_DEFAULT_MAX_SPATIAL_DEPTH UINT64_C(64)

typedef enum VpsCursorLimitResult {
    VPS_CURSOR_LIMIT_OK = 0,
    VPS_CURSOR_LIMIT_INVALID_ARGUMENT = 1,
    VPS_CURSOR_LIMIT_EXCEEDED = 2,
    VPS_CURSOR_LIMIT_OVERFLOW = 3
} VpsCursorLimitResult;

typedef struct VpsCursorLimits {
    uint64_t max_result_rows;
    uint64_t max_result_bytes;
    uint64_t max_column_bytes;
    uint64_t max_query_bytes;
    uint64_t max_identity_bytes;
    uint64_t max_parameter_bytes;
    uint64_t max_in_values;
    uint64_t max_spatial_points;
    uint64_t max_spatial_depth;
} VpsCursorLimits;

typedef struct VpsCursorBudget {
    VpsCursorLimits limits;
    uint64_t result_rows;
    uint64_t result_bytes;
    uint64_t cursor_id;
    VpsLogger *logger;
} VpsCursorBudget;

typedef struct VpsCursorMachine {
    VpsCursorState state;
    VpsCursorState waiting_from;
    uint64_t cursor_id;
    uint64_t transition_count;
    VpsLogger *logger;
} VpsCursorMachine;

VpsCursorResult vps_cursor_machine_init(VpsCursorMachine *machine,
                                        uint64_t cursor_id,
                                        VpsLogger *logger);
VpsCursorResult vps_cursor_transition(VpsCursorMachine *machine,
                                      VpsCursorEvent event);
const char *vps_cursor_state_name(VpsCursorState state);
const char *vps_cursor_event_name(VpsCursorEvent event);
void vps_cursor_limits_default(VpsCursorLimits *limits);
VpsCursorLimitResult vps_cursor_budget_init(VpsCursorBudget *budget,
                                            const VpsCursorLimits *limits,
                                            uint64_t cursor_id,
                                            VpsLogger *logger);
void vps_cursor_budget_reset(VpsCursorBudget *budget);
VpsCursorLimitResult vps_cursor_budget_check_query(
    const VpsCursorBudget *budget,
    uint64_t query_bytes,
    uint64_t parameter_bytes,
    uint64_t in_values);
VpsCursorLimitResult vps_cursor_budget_observe_row(VpsCursorBudget *budget,
                                                   uint64_t row_bytes,
                                                   uint64_t largest_column,
                                                   uint64_t identity_bytes);

#endif
