#include "vps_deadline.h"
#include "vps_error.h"
#include "vps_secure_memory.h"

#include <sqlite3.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CHECK(condition, name)                                      \
    do {                                                                 \
        if (!(condition)) {                                              \
            (void)fprintf(stderr, "support_case=%s status=failed\n",    \
                          (name));                                       \
            return 0;                                                    \
        }                                                                \
    } while (0)

typedef struct MemoryObserver {
    size_t allocations;
    size_t deallocations;
    size_t zero_before_free;
} MemoryObserver;

typedef struct PlatformFixture {
    uint64_t now_ms;
    size_t zero_calls;
    size_t zero_failures_remaining;
    size_t wait_calls;
} PlatformFixture;

typedef struct LogObserver {
    size_t event_count;
    size_t secure_events;
    size_t wait_events;
    size_t size_class_fields;
} LogObserver;

static PlatformFixture *g_platform_fixture;

static void *observer_allocate(void *context, size_t size)
{
    MemoryObserver *observer = (MemoryObserver *)context;
    void *memory = malloc(size);

    if (memory != NULL) {
        observer->allocations += 1U;
    }
    return memory;
}

static void *observer_reallocate(void *context,
                                 void *memory,
                                 size_t old_size,
                                 size_t new_size)
{
    MemoryObserver *observer = (MemoryObserver *)context;
    void *resized;

    (void)old_size;
    resized = realloc(memory, new_size);
    if (memory == NULL && resized != NULL) {
        observer->allocations += 1U;
    }
    return resized;
}

static void observer_deallocate(void *context, void *memory, size_t size)
{
    MemoryObserver *observer = (MemoryObserver *)context;
    const unsigned char *bytes = (const unsigned char *)memory;
    size_t index;
    int all_zero = 1;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            all_zero = 0;
            break;
        }
    }
    if (all_zero) {
        observer->zero_before_free += 1U;
    }
    observer->deallocations += 1U;
    free(memory);
}

static VpsPlatformStatus fixture_monotonic_now(uint64_t *milliseconds)
{
    if (g_platform_fixture == NULL || milliseconds == NULL) {
        return VPS_PLATFORM_SYSTEM_ERROR;
    }
    *milliseconds = g_platform_fixture->now_ms;
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus fixture_socket_wait(intptr_t socket_handle,
                                             VpsWaitInterest interest,
                                             uint32_t timeout_ms,
                                             VpsWaitInterest *ready_interest)
{
    (void)socket_handle;
    (void)interest;
    (void)timeout_ms;
    if (g_platform_fixture == NULL || ready_interest == NULL) {
        return VPS_PLATFORM_SYSTEM_ERROR;
    }
    g_platform_fixture->wait_calls += 1U;
    *ready_interest = (VpsWaitInterest)0;
    return VPS_PLATFORM_TIMEOUT;
}

static VpsPlatformStatus fixture_secure_zero(void *buffer, size_t buffer_size)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;
    size_t index;

    if (g_platform_fixture == NULL ||
        (buffer == NULL && buffer_size != 0U)) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    g_platform_fixture->zero_calls += 1U;
    if (g_platform_fixture->zero_failures_remaining != 0U) {
        g_platform_fixture->zero_failures_remaining -= 1U;
        return VPS_PLATFORM_SYSTEM_ERROR;
    }
    for (index = 0U; index < buffer_size; ++index) {
        bytes[index] = 0U;
    }
    return VPS_PLATFORM_OK;
}

static VpsPlatformOperations fixture_operations(void)
{
    VpsPlatformOperations operations;

    (void)memset(&operations, 0, sizeof(operations));
    operations.structure_size = (uint32_t)sizeof(operations);
    operations.contract_version = VPS_PLATFORM_CONTRACT_VERSION;
    operations.capabilities = VPS_PLATFORM_CAP_MONOTONIC_CLOCK |
                              VPS_PLATFORM_CAP_SOCKET_WAIT |
                              VPS_PLATFORM_CAP_SECURE_ZERO;
    operations.monotonic_now_ms = fixture_monotonic_now;
    operations.socket_wait = fixture_socket_wait;
    operations.secure_zero = fixture_secure_zero;
    return operations;
}

static int support_log_sink(void *context, const VpsLogEvent *event)
{
    static const char secure_operation[] = "secure_memory";
    static const char wait_operation[] = "socket_wait";
    LogObserver *observer = (LogObserver *)context;
    size_t index;

    observer->event_count += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];

        if (field->key == VPS_LOG_FIELD_SIZE_CLASS) {
            observer->size_class_fields += 1U;
        }
        if (field->key == VPS_LOG_FIELD_OPERATION &&
            field->type == VPS_LOG_FIELD_TYPE_STRING) {
            if (field->value.string_value.length ==
                    sizeof(secure_operation) - 1U &&
                memcmp(field->value.string_value.data, secure_operation,
                       sizeof(secure_operation) - 1U) == 0) {
                observer->secure_events += 1U;
            } else if (field->value.string_value.length ==
                           sizeof(wait_operation) - 1U &&
                       memcmp(field->value.string_value.data, wait_operation,
                              sizeof(wait_operation) - 1U) == 0) {
                observer->wait_events += 1U;
            }
        }
    }
    return 0;
}

static VpsInterruptProbeResult requested_interrupt(void *context)
{
    size_t *probe_count = (size_t *)context;

    *probe_count += 1U;
    return VPS_INTERRUPT_REQUESTED;
}

static int initialize_observer_allocator(MemoryObserver *observer,
                                         VpsAllocator *allocator)
{
    (void)memset(observer, 0, sizeof(*observer));
    return vps_allocator_init(allocator, VPS_ALLOCATOR_FAMILY_TEST, observer,
                              observer_allocate, observer_reallocate,
                              observer_deallocate) == VPS_MEMORY_OK;
}

static int test_secure_zero_retry(void)
{
    MemoryObserver observer;
    PlatformFixture fixture;
    VpsAllocator allocator;
    VpsPlatformOperations operations = fixture_operations();
    VpsSensitiveMemory sensitive_memory;

    (void)memset(&fixture, 0, sizeof(fixture));
    fixture.zero_failures_remaining = 1U;
    g_platform_fixture = &fixture;
    TEST_CHECK(initialize_observer_allocator(&observer, &allocator),
               "retry_allocator");
    TEST_CHECK(vps_sensitive_memory_init(&sensitive_memory, &allocator,
                                         &operations, NULL) ==
                   VPS_SECURE_MEMORY_OK &&
                   vps_sensitive_memory_allocate(&sensitive_memory, 32U) ==
                       VPS_SECURE_MEMORY_OK,
               "retry_allocate");
    (void)memset(vps_sensitive_memory_data(&sensitive_memory), 0xa5,
                 vps_sensitive_memory_size(&sensitive_memory));
    TEST_CHECK(vps_sensitive_memory_release(&sensitive_memory) ==
                   VPS_SECURE_MEMORY_ZERO_FAILED &&
                   observer.deallocations == 0U &&
                   vps_sensitive_memory_data(&sensitive_memory) != NULL,
               "retry_preserves_owner");
    TEST_CHECK(vps_sensitive_memory_release(&sensitive_memory) ==
                   VPS_SECURE_MEMORY_OK &&
                   observer.deallocations == 1U &&
                   observer.zero_before_free == 1U &&
                   fixture.zero_calls == 2U,
               "retry_zero_before_free");
    TEST_CHECK(vps_sensitive_memory_release(&sensitive_memory) ==
                   VPS_SECURE_MEMORY_OK &&
                   observer.deallocations == 1U,
               "retry_idempotent");
    return 1;
}

static int test_cancel_timeout_and_cleanup(void)
{
    MemoryObserver observer;
    PlatformFixture fixture;
    LogObserver logs;
    VpsAllocator allocator;
    VpsAllocator system_allocator;
    VpsPlatformOperations operations = fixture_operations();
    VpsSensitiveMemory sensitive_memory;
    VpsLogger logger;
    VpsError error;
    VpsDeadline deadline;
    VpsSocketWaitRequest request;
    VpsSocketWaitResult wait_result;
    size_t interrupt_count = 0U;

    (void)memset(&fixture, 0, sizeof(fixture));
    (void)memset(&logs, 0, sizeof(logs));
    fixture.now_ms = 100U;
    g_platform_fixture = &fixture;
    TEST_CHECK(initialize_observer_allocator(&observer, &allocator) &&
                   vps_allocator_system(&system_allocator) == VPS_MEMORY_OK &&
                   vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG,
                                   support_log_sink, &logs) == VPS_LOG_OK &&
                   vps_error_init(&error, &system_allocator) == VPS_MEMORY_OK,
               "combined_setup");
    TEST_CHECK(vps_sensitive_memory_init(&sensitive_memory, &allocator,
                                         &operations, &logger) ==
                   VPS_SECURE_MEMORY_OK &&
                   vps_sensitive_memory_allocate(&sensitive_memory, 24U) ==
                       VPS_SECURE_MEMORY_OK,
               "combined_sensitive_allocate");
    (void)memset(vps_sensitive_memory_data(&sensitive_memory), 0x5a,
                 vps_sensitive_memory_size(&sensitive_memory));

    TEST_CHECK(vps_deadline_init_at(100U, 20U, &deadline) ==
                   VPS_DEADLINE_OK,
               "cancel_deadline");
    (void)memset(&request, 0, sizeof(request));
    request.operations = &operations;
    request.socket_handle = (intptr_t)11;
    request.interest = VPS_WAIT_READ;
    request.deadline = &deadline;
    request.max_slice_ms = 5U;
    request.interrupt_probe = requested_interrupt;
    request.interrupt_context = &interrupt_count;
    request.logger = &logger;
    request.phase = VPS_WAIT_PHASE_CANCEL;
    TEST_CHECK(vps_socket_wait_execute(&request, &wait_result) ==
                   VPS_DEADLINE_OK &&
                   wait_result.outcome == VPS_SOCKET_WAIT_INTERRUPTED &&
                   vps_error_set_sqlstate(&error, VPS_ERROR_OPERATION_CANCEL,
                                          "57014", 0, 0, NULL) ==
                       VPS_MEMORY_OK &&
                   error.sqlite_code == SQLITE_INTERRUPT &&
                   error.error_class == VPS_ERROR_CLASS_CANCEL,
               "cancel_mapping");

    fixture.now_ms = 120U;
    request.interrupt_probe = NULL;
    request.interrupt_context = NULL;
    request.phase = VPS_WAIT_PHASE_STATEMENT;
    TEST_CHECK(vps_socket_wait_execute(&request, &wait_result) ==
                   VPS_DEADLINE_OK &&
                   wait_result.outcome == VPS_SOCKET_WAIT_DEADLINE_EXPIRED &&
                   vps_error_set_local(&error, VPS_ERROR_OPERATION_QUERY,
                                       VPS_ERROR_CLASS_TIMEOUT, NULL) ==
                       VPS_MEMORY_OK &&
                   error.sqlite_code == SQLITE_INTERRUPT &&
                   error.error_class == VPS_ERROR_CLASS_TIMEOUT &&
                   fixture.wait_calls == 0U,
               "timeout_mapping");

    TEST_CHECK(vps_sensitive_memory_release(&sensitive_memory) ==
                   VPS_SECURE_MEMORY_OK &&
                   observer.zero_before_free == 1U &&
                   observer.deallocations == 1U,
               "combined_secure_cleanup");
    vps_error_reset(&error);
    vps_error_reset(&error);
    TEST_CHECK(vps_sensitive_memory_release(&sensitive_memory) ==
                   VPS_SECURE_MEMORY_OK &&
                   logs.secure_events >= 3U && logs.wait_events == 2U &&
                   logs.size_class_fields >= 3U,
               "combined_repeated_cleanup");
    return 1;
}

static int test_fault_oom_mapping(void)
{
    PlatformFixture fixture;
    VpsAllocator system_allocator;
    VpsAllocator fault_allocator;
    VpsFaultAllocator fault;
    VpsPlatformOperations operations = fixture_operations();
    VpsSensitiveMemory sensitive_memory;
    VpsError error;

    (void)memset(&fixture, 0, sizeof(fixture));
    g_platform_fixture = &fixture;
    TEST_CHECK(vps_allocator_system(&system_allocator) == VPS_MEMORY_OK &&
                   vps_fault_allocator_init(&fault, &system_allocator, 1U) ==
                       VPS_MEMORY_OK &&
                   vps_fault_allocator_make(&fault, &fault_allocator) ==
                       VPS_MEMORY_OK &&
                   vps_sensitive_memory_init(&sensitive_memory,
                                             &fault_allocator, &operations,
                                             NULL) == VPS_SECURE_MEMORY_OK &&
                   vps_error_init(&error, &system_allocator) == VPS_MEMORY_OK,
               "oom_setup");
    TEST_CHECK(vps_sensitive_memory_allocate(&sensitive_memory, 16U) ==
                   VPS_SECURE_MEMORY_OUT_OF_MEMORY &&
                   fault.attempt_count == 1U &&
                   fault.active_allocations == 0U &&
                   vps_error_set_local(&error, VPS_ERROR_OPERATION_QUERY,
                                       VPS_ERROR_CLASS_MEMORY, NULL) ==
                       VPS_MEMORY_OK &&
                   error.sqlite_code == SQLITE_NOMEM,
               "oom_mapping");
    TEST_CHECK(vps_sensitive_memory_release(&sensitive_memory) ==
                   VPS_SECURE_MEMORY_OK,
               "oom_cleanup");
    vps_error_reset(&error);
    return 1;
}

#if defined(_WIN32)
static int test_windows_optimized_zero(void)
{
    MemoryObserver observer;
    VpsAllocator allocator;
    VpsSensitiveMemory sensitive_memory;
    const VpsPlatformOperations *operations =
        vps_platform_current_operations();

    TEST_CHECK(initialize_observer_allocator(&observer, &allocator),
               "optimized_allocator");
    TEST_CHECK(vps_sensitive_memory_init(&sensitive_memory, &allocator,
                                         operations, NULL) ==
                   VPS_SECURE_MEMORY_OK &&
                   vps_sensitive_memory_allocate(&sensitive_memory, 128U) ==
                       VPS_SECURE_MEMORY_OK,
               "optimized_allocate");
    (void)memset(vps_sensitive_memory_data(&sensitive_memory), 0xc3,
                 vps_sensitive_memory_size(&sensitive_memory));
    TEST_CHECK(vps_sensitive_memory_release(&sensitive_memory) ==
                   VPS_SECURE_MEMORY_OK &&
                   observer.deallocations == 1U &&
                   observer.zero_before_free == 1U,
               "optimized_zero_before_free");
    return 1;
}
#endif

int main(int argc, char **argv)
{
    int optimized_only = argc == 2 &&
                         strcmp(argv[1], "--optimized-zero-only") == 0;

    if (optimized_only) {
#if defined(_WIN32) && defined(VPS_OPTIMIZED_BUILD)
        if (!test_windows_optimized_zero()) {
            return 1;
        }
        (void)fprintf(stdout, "secure_zero_optimized status=passed\n");
        return 0;
#else
        (void)fprintf(stderr, "secure_zero_optimized status=unavailable\n");
        return 1;
#endif
    }
    if (!test_secure_zero_retry() || !test_cancel_timeout_and_cleanup() ||
        !test_fault_oom_mapping()) {
        return 1;
    }
#if defined(_WIN32)
    if (!test_windows_optimized_zero()) {
        return 1;
    }
#endif
    (void)fprintf(stdout, "support_integration status=passed\n");
    return 0;
}
