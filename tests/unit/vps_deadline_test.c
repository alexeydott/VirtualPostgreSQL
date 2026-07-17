#include "vps_deadline.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_CHECK(condition, name)                                      \
    do {                                                                 \
        if (!(condition)) {                                              \
            (void)fprintf(stderr, "deadline_case=%s status=failed\n",   \
                          (name));                                       \
            return 0;                                                    \
        }                                                                \
    } while (0)

#define FAKE_SEQUENCE_CAPACITY 16U

typedef struct FakeWaitState {
    uint64_t clock_values[FAKE_SEQUENCE_CAPACITY];
    size_t clock_count;
    size_t clock_index;
    VpsPlatformStatus wait_statuses[FAKE_SEQUENCE_CAPACITY];
    VpsWaitInterest wait_interests[FAKE_SEQUENCE_CAPACITY];
    size_t wait_sequence_count;
    size_t wait_index;
    VpsPlatformStatus default_wait_status;
    VpsWaitInterest default_wait_interest;
    uint32_t last_timeout_ms;
    uint64_t wait_calls;
} FakeWaitState;

typedef struct LogCapture {
    uint64_t events;
    int saw_operation;
    int saw_phase;
    int saw_duration;
    int saw_status;
} LogCapture;

static FakeWaitState *g_fake_state;

static VpsPlatformStatus fake_monotonic_now(uint64_t *milliseconds)
{
    size_t index;

    if (g_fake_state == NULL || milliseconds == NULL ||
        g_fake_state->clock_count == 0U) {
        return VPS_PLATFORM_SYSTEM_ERROR;
    }
    index = g_fake_state->clock_index;
    if (index >= g_fake_state->clock_count) {
        index = g_fake_state->clock_count - 1U;
    } else {
        g_fake_state->clock_index += 1U;
    }
    *milliseconds = g_fake_state->clock_values[index];
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus fake_socket_wait(intptr_t socket_handle,
                                          VpsWaitInterest interest,
                                          uint32_t timeout_ms,
                                          VpsWaitInterest *ready_interest)
{
    VpsPlatformStatus status;
    VpsWaitInterest ready;
    size_t index;

    (void)socket_handle;
    (void)interest;
    if (g_fake_state == NULL || ready_interest == NULL) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    g_fake_state->last_timeout_ms = timeout_ms;
    g_fake_state->wait_calls += 1U;
    index = g_fake_state->wait_index;
    if (index < g_fake_state->wait_sequence_count) {
        status = g_fake_state->wait_statuses[index];
        ready = g_fake_state->wait_interests[index];
        g_fake_state->wait_index += 1U;
    } else {
        status = g_fake_state->default_wait_status;
        ready = g_fake_state->default_wait_interest;
    }
    *ready_interest = ready;
    return status;
}

static VpsPlatformOperations fake_operations(void)
{
    VpsPlatformOperations operations;

    (void)memset(&operations, 0, sizeof(operations));
    operations.structure_size = (uint32_t)sizeof(operations);
    operations.contract_version = VPS_PLATFORM_CONTRACT_VERSION;
    operations.capabilities = VPS_PLATFORM_CAP_MONOTONIC_CLOCK |
                              VPS_PLATFORM_CAP_SOCKET_WAIT;
    operations.monotonic_now_ms = fake_monotonic_now;
    operations.socket_wait = fake_socket_wait;
    return operations;
}

static VpsSocketWaitRequest fake_request(
    const VpsPlatformOperations *operations,
    const VpsDeadline *deadline)
{
    VpsSocketWaitRequest request;

    (void)memset(&request, 0, sizeof(request));
    request.operations = operations;
    request.socket_handle = (intptr_t)17;
    request.interest = VPS_WAIT_READ;
    request.deadline = deadline;
    request.max_slice_ms = 10U;
    request.phase = VPS_WAIT_PHASE_STATEMENT;
    return request;
}

static VpsInterruptProbeResult interrupt_requested(void *context)
{
    uint64_t *calls = (uint64_t *)context;

    *calls += 1U;
    return VPS_INTERRUPT_REQUESTED;
}

static int capture_log(void *context, const VpsLogEvent *event)
{
    LogCapture *capture = (LogCapture *)context;
    size_t index;

    capture->events += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        switch (event->fields[index].key) {
        case VPS_LOG_FIELD_OPERATION:
            capture->saw_operation = 1;
            break;
        case VPS_LOG_FIELD_PHASE:
            capture->saw_phase = 1;
            break;
        case VPS_LOG_FIELD_DURATION_MS:
            capture->saw_duration = 1;
            break;
        case VPS_LOG_FIELD_STATUS:
            capture->saw_status = 1;
            break;
        default:
            return 1;
        }
    }
    return 0;
}

static int test_deadline_math(void)
{
    VpsDeadline deadline;
    uint64_t remaining;
    int expired;

    TEST_CHECK(vps_deadline_init_at(100U, 25U, &deadline) ==
                   VPS_DEADLINE_OK &&
                   deadline.expires_at_ms == 125U,
               "checked_add");
    TEST_CHECK(vps_deadline_remaining_at(&deadline, 124U, &remaining,
                                         &expired) == VPS_DEADLINE_OK &&
                   remaining == 1U && !expired,
               "remaining");
    TEST_CHECK(vps_deadline_remaining_at(&deadline, 125U, &remaining,
                                         &expired) == VPS_DEADLINE_OK &&
                   remaining == 0U && expired,
               "exact_boundary");
    TEST_CHECK(vps_deadline_init_at(UINT64_MAX, 1U, &deadline) ==
                   VPS_DEADLINE_OVERFLOW,
               "overflow");
    TEST_CHECK(vps_deadline_init_at(100U, 1U, &deadline) ==
                   VPS_DEADLINE_OK &&
                   vps_deadline_remaining_at(&deadline, 99U, &remaining,
                                              &expired) ==
                       VPS_DEADLINE_CLOCK_ERROR,
               "clock_wrap");
    return 1;
}

static int test_expired_without_wait(void)
{
    FakeWaitState state;
    VpsPlatformOperations operations = fake_operations();
    VpsDeadline deadline;
    VpsSocketWaitRequest request;
    VpsSocketWaitResult result;

    (void)memset(&state, 0, sizeof(state));
    state.clock_values[0] = 200U;
    state.clock_count = 1U;
    state.default_wait_status = VPS_PLATFORM_OK;
    state.default_wait_interest = VPS_WAIT_READ;
    g_fake_state = &state;
    TEST_CHECK(vps_deadline_init_at(100U, 50U, &deadline) ==
                   VPS_DEADLINE_OK,
               "expired_setup");
    request = fake_request(&operations, &deadline);
    TEST_CHECK(vps_socket_wait_execute(&request, &result) == VPS_DEADLINE_OK &&
                   result.outcome == VPS_SOCKET_WAIT_DEADLINE_EXPIRED &&
                   result.wait_count == 0U && state.wait_calls == 0U,
               "expired_immediate");
    return 1;
}

static int test_spurious_then_ready_and_logging(void)
{
    FakeWaitState state;
    LogCapture capture;
    VpsLogger logger;
    VpsPlatformOperations operations = fake_operations();
    VpsDeadline deadline;
    VpsSocketWaitRequest request;
    VpsSocketWaitResult result;

    (void)memset(&state, 0, sizeof(state));
    (void)memset(&capture, 0, sizeof(capture));
    state.clock_values[0] = 100U;
    state.clock_values[1] = 101U;
    state.clock_count = 2U;
    state.wait_statuses[0] = VPS_PLATFORM_OK;
    state.wait_interests[0] = (VpsWaitInterest)0;
    state.wait_statuses[1] = VPS_PLATFORM_OK;
    state.wait_interests[1] = VPS_WAIT_READ;
    state.wait_sequence_count = 2U;
    g_fake_state = &state;
    TEST_CHECK(vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG, capture_log,
                               &capture) == VPS_LOG_OK,
               "logger_setup");
    TEST_CHECK(vps_deadline_init_at(100U, 20U, &deadline) ==
                   VPS_DEADLINE_OK,
               "spurious_setup");
    request = fake_request(&operations, &deadline);
    request.logger = &logger;
    TEST_CHECK(vps_socket_wait_execute(&request, &result) == VPS_DEADLINE_OK &&
                   result.outcome == VPS_SOCKET_WAIT_READY &&
                   result.wait_count == 2U &&
                   result.spurious_wakeup_count == 1U &&
                   result.ready_interest == VPS_WAIT_READ,
               "spurious_ready");
    TEST_CHECK(capture.events == 1U && capture.saw_operation &&
                   capture.saw_phase && capture.saw_duration &&
                   capture.saw_status,
               "structured_log");
    return 1;
}

static int test_interrupt_before_clock(void)
{
    FakeWaitState state;
    VpsPlatformOperations operations = fake_operations();
    VpsDeadline deadline;
    VpsSocketWaitRequest request;
    VpsSocketWaitResult result;
    uint64_t probe_calls = 0U;

    (void)memset(&state, 0, sizeof(state));
    state.clock_values[0] = 100U;
    state.clock_count = 1U;
    g_fake_state = &state;
    TEST_CHECK(vps_deadline_init_at(100U, 20U, &deadline) ==
                   VPS_DEADLINE_OK,
               "interrupt_setup");
    request = fake_request(&operations, &deadline);
    request.interrupt_probe = interrupt_requested;
    request.interrupt_context = &probe_calls;
    TEST_CHECK(vps_socket_wait_execute(&request, &result) == VPS_DEADLINE_OK &&
                   result.outcome == VPS_SOCKET_WAIT_INTERRUPTED &&
                   result.wait_count == 0U && state.clock_index == 0U &&
                   probe_calls == 1U,
               "interrupt_preempts_wait");
    return 1;
}

static int test_timeout_slice_and_clock_progress(void)
{
    FakeWaitState state;
    VpsPlatformOperations operations = fake_operations();
    VpsDeadline deadline;
    VpsSocketWaitRequest request;
    VpsSocketWaitResult result;

    (void)memset(&state, 0, sizeof(state));
    state.clock_values[0] = 100U;
    state.clock_values[1] = 103U;
    state.clock_values[2] = 105U;
    state.clock_count = 3U;
    state.default_wait_status = VPS_PLATFORM_TIMEOUT;
    g_fake_state = &state;
    TEST_CHECK(vps_deadline_init_at(100U, 5U, &deadline) ==
                   VPS_DEADLINE_OK,
               "timeout_setup");
    request = fake_request(&operations, &deadline);
    request.max_slice_ms = 3U;
    TEST_CHECK(vps_socket_wait_execute(&request, &result) == VPS_DEADLINE_OK &&
                   result.outcome == VPS_SOCKET_WAIT_DEADLINE_EXPIRED &&
                   result.wait_count == 2U && state.last_timeout_ms == 2U &&
                   state.last_timeout_ms <= (uint32_t)INT_MAX,
               "windows_timeout_mapping");
    return 1;
}

static int test_clock_regression_and_retry_limit(void)
{
    FakeWaitState state;
    VpsPlatformOperations operations = fake_operations();
    VpsDeadline deadline;
    VpsSocketWaitRequest request;
    VpsSocketWaitResult result;

    (void)memset(&state, 0, sizeof(state));
    state.clock_values[0] = 100U;
    state.clock_values[1] = 99U;
    state.clock_count = 2U;
    state.default_wait_status = VPS_PLATFORM_OK;
    state.default_wait_interest = (VpsWaitInterest)0;
    g_fake_state = &state;
    TEST_CHECK(vps_deadline_init_at(100U, 100U, &deadline) ==
                   VPS_DEADLINE_OK,
               "regression_setup");
    request = fake_request(&operations, &deadline);
    TEST_CHECK(vps_socket_wait_execute(&request, &result) ==
                   VPS_DEADLINE_CLOCK_ERROR &&
                   result.outcome == VPS_SOCKET_WAIT_FAILED,
               "clock_regression");

    (void)memset(&state, 0, sizeof(state));
    state.clock_values[0] = 100U;
    state.clock_count = 1U;
    state.default_wait_status = VPS_PLATFORM_OK;
    state.default_wait_interest = (VpsWaitInterest)0;
    g_fake_state = &state;
    request = fake_request(&operations, &deadline);
    TEST_CHECK(vps_socket_wait_execute(&request, &result) ==
                   VPS_DEADLINE_RETRY_LIMIT &&
                   result.spurious_wakeup_count ==
                       VPS_WAIT_MAX_SPURIOUS_WAKEUPS,
               "spurious_retry_limit");
    return 1;
}

int main(void)
{
    if (!test_deadline_math() || !test_expired_without_wait() ||
        !test_spurious_then_ready_and_logging() ||
        !test_interrupt_before_clock() ||
        !test_timeout_slice_and_clock_progress() ||
        !test_clock_regression_and_retry_limit()) {
        return 1;
    }
    (void)fprintf(stdout, "deadline_suite status=passed\n");
    return 0;
}
