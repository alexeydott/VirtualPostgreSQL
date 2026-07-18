#ifndef VPS_DEADLINE_H
#define VPS_DEADLINE_H

#include "vps_logging.h"
#include "vps_platform.h"

#include <stdint.h>

#define VPS_WAIT_MAX_SLICE_MS UINT32_C(1000)
#define VPS_WAIT_MAX_SPURIOUS_WAKEUPS UINT32_C(64)

typedef enum VpsDeadlineStatus {
    VPS_DEADLINE_OK = 0,
    VPS_DEADLINE_INVALID_ARGUMENT = 1,
    VPS_DEADLINE_OVERFLOW = 2,
    VPS_DEADLINE_CLOCK_ERROR = 3,
    VPS_DEADLINE_WAIT_ERROR = 4,
    VPS_DEADLINE_INTERRUPT_ERROR = 5,
    VPS_DEADLINE_RETRY_LIMIT = 6
} VpsDeadlineStatus;

typedef enum VpsWaitPhase {
    VPS_WAIT_PHASE_CONNECT = 0,
    VPS_WAIT_PHASE_STATEMENT = 1,
    VPS_WAIT_PHASE_LOCK = 2,
    VPS_WAIT_PHASE_POOL = 3,
    VPS_WAIT_PHASE_FETCH_IDLE = 4,
    VPS_WAIT_PHASE_RESET = 5,
    VPS_WAIT_PHASE_PING = 6,
    VPS_WAIT_PHASE_CANCEL = 7
} VpsWaitPhase;

typedef enum VpsInterruptProbeResult {
    VPS_INTERRUPT_CONTINUE = 0,
    VPS_INTERRUPT_REQUESTED = 1,
    VPS_INTERRUPT_PROBE_ERROR = 2
} VpsInterruptProbeResult;

typedef enum VpsSocketWaitOutcome {
    VPS_SOCKET_WAIT_READY = 0,
    VPS_SOCKET_WAIT_DEADLINE_EXPIRED = 1,
    VPS_SOCKET_WAIT_INTERRUPTED = 2,
    VPS_SOCKET_WAIT_FAILED = 3
} VpsSocketWaitOutcome;

typedef struct VpsDeadline {
    uint64_t started_at_ms;
    uint64_t expires_at_ms;
} VpsDeadline;

typedef VpsInterruptProbeResult (*VpsInterruptProbe)(void *context);

/*
 * A wait request borrows every pointer for the synchronous call. The logger is
 * optional and must obey its caller-serialized contract. max_slice_ms must be
 * in [1, VPS_WAIT_MAX_SLICE_MS], so interrupt polling is always bounded.
 */
typedef struct VpsSocketWaitRequest {
    const VpsPlatformOperations *operations;
    intptr_t socket_handle;
    VpsWaitInterest interest;
    const VpsDeadline *deadline;
    uint32_t max_slice_ms;
    VpsInterruptProbe interrupt_probe;
    void *interrupt_context;
    VpsLogger *logger;
    VpsWaitPhase phase;
} VpsSocketWaitRequest;

typedef struct VpsSocketWaitResult {
    VpsSocketWaitOutcome outcome;
    VpsWaitInterest ready_interest;
    VpsPlatformStatus platform_status;
    uint64_t remaining_ms;
    uint64_t wait_count;
    uint64_t spurious_wakeup_count;
} VpsSocketWaitResult;

VpsDeadlineStatus vps_deadline_init_at(uint64_t now_ms,
                                       uint64_t duration_ms,
                                       VpsDeadline *deadline);
VpsDeadlineStatus vps_deadline_start(
    const VpsPlatformOperations *operations,
    uint64_t duration_ms,
    VpsDeadline *deadline);
VpsDeadlineStatus vps_deadline_remaining_at(const VpsDeadline *deadline,
                                            uint64_t now_ms,
                                            uint64_t *remaining_ms,
                                            int *expired);
VpsDeadlineStatus vps_socket_wait_execute(
    const VpsSocketWaitRequest *request,
    VpsSocketWaitResult *result);

const char *vps_wait_phase_name(VpsWaitPhase phase);
const char *vps_socket_wait_outcome_name(VpsSocketWaitOutcome outcome);

#endif
