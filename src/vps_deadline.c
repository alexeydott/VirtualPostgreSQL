#include "vps_deadline.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

_Static_assert(VPS_WAIT_MAX_SLICE_MS <= INT_MAX,
               "socket wait slices must fit the Windows timeout type");

static int vps_wait_interest_is_valid(VpsWaitInterest interest)
{
    return interest == VPS_WAIT_READ || interest == VPS_WAIT_WRITE ||
           interest == VPS_WAIT_READ_WRITE;
}

static int vps_wait_phase_is_valid(VpsWaitPhase phase)
{
    return phase >= VPS_WAIT_PHASE_CONNECT && phase <= VPS_WAIT_PHASE_CANCEL;
}

static void vps_wait_log(VpsLogger *logger,
                         VpsLogLevel level,
                         VpsWaitPhase phase,
                         uint64_t remaining_ms,
                         const char *status)
{
    static const char operation[] = "socket_wait";
    VpsLogEvent event;
    const char *phase_name = vps_wait_phase_name(phase);

    if (logger == NULL || status == NULL ||
        vps_log_event_init(&event, level) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE, phase_name,
                                 vps_wait_phase_is_valid(phase)
                                     ? strlen(phase_name)
                                     : 0U) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_DURATION_MS,
                                 remaining_ms) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK) {
        return;
    }
    vps_logger_emit(logger, &event);
}

static void vps_wait_result_init(VpsSocketWaitResult *result)
{
    result->outcome = VPS_SOCKET_WAIT_FAILED;
    result->ready_interest = (VpsWaitInterest)0;
    result->platform_status = VPS_PLATFORM_OK;
    result->remaining_ms = 0U;
    result->wait_count = 0U;
    result->spurious_wakeup_count = 0U;
}

VpsDeadlineStatus vps_deadline_init_at(uint64_t now_ms,
                                       uint64_t duration_ms,
                                       VpsDeadline *deadline)
{
    VpsDeadline initialized;

    if (deadline == NULL) {
        return VPS_DEADLINE_INVALID_ARGUMENT;
    }
    if (duration_ms > UINT64_MAX - now_ms) {
        return VPS_DEADLINE_OVERFLOW;
    }
    initialized.started_at_ms = now_ms;
    initialized.expires_at_ms = now_ms + duration_ms;
    *deadline = initialized;
    return VPS_DEADLINE_OK;
}

VpsDeadlineStatus vps_deadline_start(
    const VpsPlatformOperations *operations,
    uint64_t duration_ms,
    VpsDeadline *deadline)
{
    uint64_t now_ms;

    if (operations == NULL || deadline == NULL) {
        return VPS_DEADLINE_INVALID_ARGUMENT;
    }
    if (vps_platform_monotonic_now_ms(operations, &now_ms) !=
        VPS_PLATFORM_OK) {
        return VPS_DEADLINE_CLOCK_ERROR;
    }
    return vps_deadline_init_at(now_ms, duration_ms, deadline);
}

VpsDeadlineStatus vps_deadline_remaining_at(const VpsDeadline *deadline,
                                            uint64_t now_ms,
                                            uint64_t *remaining_ms,
                                            int *expired)
{
    if (deadline == NULL || remaining_ms == NULL || expired == NULL ||
        deadline->expires_at_ms < deadline->started_at_ms) {
        return VPS_DEADLINE_INVALID_ARGUMENT;
    }
    if (now_ms < deadline->started_at_ms) {
        return VPS_DEADLINE_CLOCK_ERROR;
    }
    if (now_ms >= deadline->expires_at_ms) {
        *remaining_ms = 0U;
        *expired = 1;
    } else {
        *remaining_ms = deadline->expires_at_ms - now_ms;
        *expired = 0;
    }
    return VPS_DEADLINE_OK;
}

VpsDeadlineStatus vps_socket_wait_execute(
    const VpsSocketWaitRequest *request,
    VpsSocketWaitResult *result)
{
    uint64_t previous_now = 0U;
    int have_previous_now = 0;
    int require_clock_progress = 0;

    if (request == NULL || result == NULL || request->operations == NULL ||
        request->deadline == NULL ||
        !vps_wait_interest_is_valid(request->interest) ||
        !vps_wait_phase_is_valid(request->phase) ||
        request->max_slice_ms == 0U ||
        request->max_slice_ms > VPS_WAIT_MAX_SLICE_MS ||
        vps_platform_validate_operations(
            request->operations, VPS_PLATFORM_CAP_MONOTONIC_CLOCK |
                                     VPS_PLATFORM_CAP_SOCKET_WAIT) !=
            VPS_PLATFORM_OK) {
        return VPS_DEADLINE_INVALID_ARGUMENT;
    }

    vps_wait_result_init(result);
    for (;;) {
        VpsInterruptProbeResult interrupt_result = VPS_INTERRUPT_CONTINUE;
        VpsWaitInterest ready_interest = (VpsWaitInterest)0;
        VpsPlatformStatus platform_status;
        VpsDeadlineStatus deadline_status;
        uint64_t now_ms;
        uint64_t remaining_ms;
        uint32_t timeout_ms;
        int expired;

        if (request->interrupt_probe != NULL) {
            interrupt_result =
                request->interrupt_probe(request->interrupt_context);
        }
        if (interrupt_result == VPS_INTERRUPT_REQUESTED) {
            result->outcome = VPS_SOCKET_WAIT_INTERRUPTED;
            vps_wait_log(request->logger, VPS_LOG_LEVEL_INFO, request->phase,
                         result->remaining_ms, "interrupted");
            return VPS_DEADLINE_OK;
        }
        if (interrupt_result != VPS_INTERRUPT_CONTINUE) {
            result->outcome = VPS_SOCKET_WAIT_FAILED;
            vps_wait_log(request->logger, VPS_LOG_LEVEL_ERROR, request->phase,
                         result->remaining_ms, "interrupt_error");
            return VPS_DEADLINE_INTERRUPT_ERROR;
        }

        platform_status = vps_platform_monotonic_now_ms(request->operations,
                                                        &now_ms);
        result->platform_status = platform_status;
        if (platform_status != VPS_PLATFORM_OK ||
            (have_previous_now && now_ms < previous_now) ||
            (require_clock_progress && now_ms <= previous_now)) {
            result->outcome = VPS_SOCKET_WAIT_FAILED;
            vps_wait_log(request->logger, VPS_LOG_LEVEL_ERROR, request->phase,
                         result->remaining_ms, "clock_error");
            return VPS_DEADLINE_CLOCK_ERROR;
        }
        previous_now = now_ms;
        have_previous_now = 1;
        require_clock_progress = 0;

        deadline_status = vps_deadline_remaining_at(
            request->deadline, now_ms, &remaining_ms, &expired);
        if (deadline_status != VPS_DEADLINE_OK) {
            result->outcome = VPS_SOCKET_WAIT_FAILED;
            vps_wait_log(request->logger, VPS_LOG_LEVEL_ERROR, request->phase,
                         result->remaining_ms, "clock_error");
            return deadline_status;
        }
        result->remaining_ms = remaining_ms;
        if (expired) {
            result->outcome = VPS_SOCKET_WAIT_DEADLINE_EXPIRED;
            result->platform_status = VPS_PLATFORM_TIMEOUT;
            vps_wait_log(request->logger, VPS_LOG_LEVEL_INFO, request->phase,
                         0U, "deadline_expired");
            return VPS_DEADLINE_OK;
        }

        timeout_ms = remaining_ms > request->max_slice_ms
                         ? request->max_slice_ms
                         : (uint32_t)remaining_ms;
        platform_status = vps_platform_socket_wait(
            request->operations, request->socket_handle, request->interest,
            timeout_ms, &ready_interest);
        result->platform_status = platform_status;
        result->wait_count += 1U;

        if (platform_status == VPS_PLATFORM_OK &&
            ((int)ready_interest & (int)request->interest) != 0) {
            result->outcome = VPS_SOCKET_WAIT_READY;
            result->ready_interest =
                (VpsWaitInterest)((int)ready_interest & (int)request->interest);
            vps_wait_log(request->logger, VPS_LOG_LEVEL_DEBUG, request->phase,
                         remaining_ms, "ready");
            return VPS_DEADLINE_OK;
        }
        if (platform_status != VPS_PLATFORM_OK &&
            platform_status != VPS_PLATFORM_TIMEOUT) {
            result->outcome = VPS_SOCKET_WAIT_FAILED;
            vps_wait_log(request->logger, VPS_LOG_LEVEL_ERROR, request->phase,
                         remaining_ms, "wait_error");
            return VPS_DEADLINE_WAIT_ERROR;
        }
        if (platform_status == VPS_PLATFORM_OK) {
            result->spurious_wakeup_count += 1U;
            if (result->spurious_wakeup_count >=
                VPS_WAIT_MAX_SPURIOUS_WAKEUPS) {
                result->outcome = VPS_SOCKET_WAIT_FAILED;
                vps_wait_log(request->logger, VPS_LOG_LEVEL_ERROR,
                             request->phase, remaining_ms, "retry_limit");
                return VPS_DEADLINE_RETRY_LIMIT;
            }
        } else {
            require_clock_progress = 1;
        }
    }
}

const char *vps_wait_phase_name(VpsWaitPhase phase)
{
    static const char *const names[] = {"connect", "statement", "lock",
                                        "pool", "fetch_idle", "cancel"};

    if (!vps_wait_phase_is_valid(phase)) {
        return "unknown";
    }
    return names[(size_t)phase];
}

const char *vps_socket_wait_outcome_name(VpsSocketWaitOutcome outcome)
{
    static const char *const names[] = {"ready", "deadline_expired",
                                        "interrupted", "failed"};

    if (outcome < VPS_SOCKET_WAIT_READY ||
        outcome > VPS_SOCKET_WAIT_FAILED) {
        return "unknown";
    }
    return names[(size_t)outcome];
}
