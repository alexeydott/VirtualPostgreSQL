#include "vps_credential_provider.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

typedef enum VpsTestProviderMode {
    VPS_TEST_PROVIDER_VALID = 0,
    VPS_TEST_PROVIDER_FAIL = 1,
    VPS_TEST_PROVIDER_PARTIAL = 2,
    VPS_TEST_PROVIDER_SHORT_LEASE = 3,
    VPS_TEST_PROVIDER_OLD_CONFIG = 4,
    VPS_TEST_PROVIDER_LARGE_CONFIG = 5,
    VPS_TEST_PROVIDER_OLD_LEASE = 6,
    VPS_TEST_PROVIDER_LARGE_LEASE = 7,
    VPS_TEST_PROVIDER_BLOCK = 8
} VpsTestProviderMode;

typedef struct VpsTestProviderState {
    VpsCredentialConfig config;
    VpsTestProviderMode mode;
    volatile long resolve_count;
    volatile long release_count;
    volatile long active_count;
    volatile long maximum_active;
    long block_target;
#if defined(_WIN32)
    HANDLE entered_event;
    HANDLE continue_event;
#endif
} VpsTestProviderState;

typedef struct VpsTestLogState {
    size_t events;
    int unsafe_value_seen;
    uint64_t last_provider_id;
    uint64_t last_generation;
} VpsTestLogState;

typedef struct VpsWipeAllocatorState {
    size_t deallocate_count;
    int nonzero_byte_seen;
} VpsWipeAllocatorState;

static void *vps_wipe_allocate(void *context, size_t size)
{
    (void)context;
    return malloc(size);
}

static void *vps_wipe_reallocate(void *context,
                                 void *memory,
                                 size_t old_size,
                                 size_t new_size)
{
    (void)context;
    (void)old_size;
    return realloc(memory, new_size);
}

static void vps_wipe_deallocate(void *context, void *memory, size_t size)
{
    VpsWipeAllocatorState *state = (VpsWipeAllocatorState *)context;
    const unsigned char *bytes = (const unsigned char *)memory;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            state->nonzero_byte_seen = 1;
            break;
        }
    }
    state->deallocate_count += 1U;
    free(memory);
}

static long vps_test_increment(volatile long *value)
{
#if defined(_WIN32)
    return InterlockedIncrement(value);
#else
    *value += 1;
    return *value;
#endif
}

static long vps_test_decrement(volatile long *value)
{
#if defined(_WIN32)
    return InterlockedDecrement(value);
#else
    *value -= 1;
    return *value;
#endif
}

static void vps_test_update_maximum(volatile long *maximum, long value)
{
#if defined(_WIN32)
    long observed = *maximum;
    while (value > observed) {
        long previous = InterlockedCompareExchange(maximum, value, observed);
        if (previous == observed) {
            break;
        }
        observed = previous;
    }
#else
    if (value > *maximum) {
        *maximum = value;
    }
#endif
}

static int32_t VPS_CALL vps_test_provider_resolve(
    void *provider_context,
    const char *credential_ref,
    uint32_t credential_ref_length,
    VpsCredentialLease *lease)
{
    VpsTestProviderState *state = (VpsTestProviderState *)provider_context;
    long active;

    (void)credential_ref;
    (void)credential_ref_length;
    (void)vps_test_increment(&state->resolve_count);
    if (state->mode == VPS_TEST_PROVIDER_FAIL) {
        return VPS_CREDENTIAL_PROVIDER_NOT_FOUND;
    }
    active = vps_test_increment(&state->active_count);
    vps_test_update_maximum(&state->maximum_active, active);
#if defined(_WIN32)
    if (state->mode == VPS_TEST_PROVIDER_BLOCK) {
        if (active >= state->block_target) {
            (void)SetEvent(state->entered_event);
        }
        (void)WaitForSingleObject(state->continue_event, 5000U);
    }
#endif
    if (state->mode == VPS_TEST_PROVIDER_SHORT_LEASE) {
        lease->header.structure_size = (uint32_t)sizeof(VpsAbiHeader);
        lease->config = &state->config;
        (void)vps_test_decrement(&state->active_count);
        return VPS_CREDENTIAL_PROVIDER_OK;
    }
    lease->header.structure_size = (uint32_t)sizeof(*lease);
    lease->header.api_version = VPS_API_VERSION;
    lease->header.present_fields = VPS_CREDENTIAL_LEASE_FIELDS_CURRENT;
    if (state->mode == VPS_TEST_PROVIDER_OLD_LEASE) {
        lease->header.structure_size =
            (uint32_t)(offsetof(VpsCredentialLease, provider_lease) +
                       sizeof(lease->provider_lease));
        lease->header.present_fields = 0U;
    } else if (state->mode == VPS_TEST_PROVIDER_LARGE_LEASE) {
        lease->header.structure_size =
            (uint32_t)(sizeof(*lease) + sizeof(uintptr_t) * 4U);
        lease->header.present_fields |= UINT64_C(1) << 62;
    }
    lease->config = &state->config;
    lease->provider_lease = state;
    (void)vps_test_decrement(&state->active_count);
    return VPS_CREDENTIAL_PROVIDER_OK;
}

static void VPS_CALL vps_test_provider_release(void *provider_context,
                                               VpsCredentialLease *lease)
{
    VpsTestProviderState *state = (VpsTestProviderState *)provider_context;
    (void)vps_test_increment(&state->release_count);
    (void)memset(lease, 0, sizeof(*lease));
}

static void vps_test_state_init(VpsTestProviderState *state)
{
    (void)memset(state, 0, sizeof(*state));
    state->config.header.structure_size = (uint32_t)sizeof(state->config);
    state->config.header.api_version = VPS_API_VERSION;
    state->config.header.present_fields =
        VPS_CREDENTIAL_FIELD_HOSTS | VPS_CREDENTIAL_FIELD_USER |
        VPS_CREDENTIAL_FIELD_PASSWORD | VPS_CREDENTIAL_FIELD_SSLMODE;
    state->config.hosts = "db.internal";
    state->config.user = "readonly";
    state->config.password = "s3ns1t1ve";
    state->config.sslmode = "verify-full";
}

static VpsCredentialProvider vps_test_provider(VpsTestProviderState *state)
{
    VpsCredentialProvider provider;
    (void)memset(&provider, 0, sizeof(provider));
    provider.header.structure_size = (uint32_t)sizeof(provider);
    provider.header.api_version = VPS_API_VERSION;
    provider.header.present_fields = VPS_CREDENTIAL_PROVIDER_FIELDS_CURRENT;
    provider.resolve = vps_test_provider_resolve;
    provider.release = vps_test_provider_release;
    provider.provider_context = state;
    return provider;
}

static int vps_expect(int condition, const char *case_name)
{
    if (!condition) {
        (void)fprintf(stderr,
                      "[credential_provider] level=error case=%s status=failed\n",
                      case_name);
        return 0;
    }
    return 1;
}

static int vps_test_log_sink(void *context, const VpsLogEvent *event)
{
    VpsTestLogState *state = (VpsTestLogState *)context;
    size_t index;
    state->events += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            (memchr(field->value.string_value.data, '!',
                    field->value.string_value.length) != NULL ||
             memchr(field->value.string_value.data, '\\',
                    field->value.string_value.length) != NULL)) {
            state->unsafe_value_seen = 1;
        }
        if (field->key == VPS_LOG_FIELD_PROVIDER_ID) {
            state->last_provider_id = field->value.uint64_value;
        } else if (field->key == VPS_LOG_FIELD_GENERATION) {
            state->last_generation = field->value.uint64_value;
        }
    }
    return 0;
}

static int vps_test_registration_and_abi(void)
{
    const VpsPlatformOperations *operations = vps_platform_current_operations();
    VpsCredentialRegistry registry = {0};
    VpsTestProviderState first_state;
    VpsTestProviderState second_state;
    VpsCredentialProvider first;
    VpsCredentialProvider second;
    struct {
        VpsCredentialProvider provider;
        uintptr_t future[8];
    } larger;
    int passed = 1;

    vps_test_state_init(&first_state);
    vps_test_state_init(&second_state);
    first = vps_test_provider(&first_state);
    second = vps_test_provider(&second_state);
    passed &= vps_expect(vps_credential_registry_init(
                             &registry, operations, NULL) ==
                             VPS_CREDENTIAL_REGISTRY_OK,
                         "registry_init");

    first.header.structure_size =
        (uint32_t)(offsetof(VpsCredentialProvider, provider_context) +
                   sizeof(first.provider_context));
    first.header.present_fields = 0U;
    passed &= vps_expect(vps_credential_registry_register(&registry, 11U,
                                                           &first) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             registry.generation == 1U,
                         "legacy_provider_registration");
    passed &= vps_expect(vps_credential_registry_register(&registry, 12U,
                                                           &second) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             registry.generation == 2U,
                         "replacement_before_resolve");

    (void)memset(&larger, 0, sizeof(larger));
    larger.provider = second;
    larger.provider.header.structure_size = (uint32_t)sizeof(larger);
    larger.provider.header.present_fields |= UINT64_C(1) << 60;
    passed &= vps_expect(vps_credential_registry_register(
                             &registry, 13U, &larger.provider) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             registry.generation == 3U,
                         "larger_provider_registration");

    larger.provider.resolve = NULL;
    passed &= vps_expect(vps_credential_registry_register(
                             &registry, 14U, &larger.provider) ==
                             VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT,
                         "missing_resolve_rejected");
    larger.provider = second;
    larger.provider.header.structure_size = (uint32_t)sizeof(VpsAbiHeader);
    passed &= vps_expect(vps_credential_registry_register(
                             &registry, 14U, &larger.provider) ==
                             VPS_CREDENTIAL_REGISTRY_ABI_INCOMPATIBLE,
                         "short_provider_rejected");
    passed &= vps_expect(vps_credential_registry_cleanup(&registry) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_credential_registry_cleanup(&registry) ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                         "registry_cleanup_repeat");
    return passed;
}

static int vps_test_resolve_copy_release_and_logging(void)
{
    const VpsPlatformOperations *operations = vps_platform_current_operations();
    VpsAllocator allocator;
    VpsCredentialRegistry registry = {0};
    VpsResolvedCredential resolved = {0};
    VpsTestProviderState state;
    VpsCredentialProvider provider;
    VpsTestLogState log_state = {0};
    VpsLogger logger;
    VpsWipeAllocatorState wipe_state = {0};
    int passed = 1;

    vps_test_state_init(&state);
    provider = vps_test_provider(&state);
    passed &= vps_expect(vps_allocator_init(
                             &allocator, VPS_ALLOCATOR_FAMILY_TEST,
                             &wipe_state, vps_wipe_allocate,
                             vps_wipe_reallocate, vps_wipe_deallocate) ==
                             VPS_MEMORY_OK &&
                             vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG,
                                             vps_test_log_sink, &log_state) ==
                                 VPS_LOG_OK &&
                             vps_credential_registry_init(&registry, operations,
                                                          &logger) ==
                                 VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_credential_registry_register(&registry, 91U,
                                                              &provider) ==
                                 VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_resolved_credential_init(
                                 &resolved, &allocator, operations, &logger) ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                         "resolve_fixture_init");
    passed &= vps_expect(vps_credential_registry_resolve(
                             &registry, "unsafe!ref", 10U, &resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             state.resolve_count == 1 && state.release_count == 1,
                         "resolve_release_exactly_once");
    passed &= vps_expect(resolved.provider_id == 91U &&
                             resolved.generation == 1U &&
                             strcmp(resolved.config.password, "s3ns1t1ve") == 0 &&
                             resolved.config.password != state.config.password,
                         "resolved_config_owned_copy");
    state.config.password = "changed";
    passed &= vps_expect(strcmp(resolved.config.password, "s3ns1t1ve") == 0,
                         "provider_mutation_does_not_change_copy");
    passed &= vps_expect(vps_credential_registry_register(&registry, 92U,
                                                           &provider) ==
                             VPS_CREDENTIAL_REGISTRY_REPLACEMENT_FORBIDDEN,
                         "replacement_after_resolve_forbidden");
    passed &= vps_expect(log_state.events >= 3U && !log_state.unsafe_value_seen &&
                             log_state.last_provider_id == 91U &&
                             log_state.last_generation == 1U,
                         "structured_logs_are_value_free");
    passed &= vps_expect(vps_resolved_credential_cleanup(&resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_resolved_credential_cleanup(&resolved) ==
                                 VPS_CREDENTIAL_REGISTRY_OK &&
                             wipe_state.deallocate_count == 1U &&
                             !wipe_state.nonzero_byte_seen,
                         "resolved_cleanup_repeat");
    passed &= vps_expect(vps_credential_registry_cleanup(&registry) ==
                             VPS_CREDENTIAL_REGISTRY_OK,
                         "resolve_registry_cleanup");
    return passed;
}

static int vps_test_failure_matrix(void)
{
    const VpsPlatformOperations *operations = vps_platform_current_operations();
    VpsAllocator system_allocator;
    VpsAllocator fault_allocator;
    VpsFaultAllocator fault;
    VpsCredentialRegistry registry = {0};
    VpsResolvedCredential resolved = {0};
    VpsTestProviderState state;
    VpsCredentialProvider provider;
    static char oversize_value[VPS_CREDENTIAL_VALUE_MAX_LENGTH + 2U];
    int passed = 1;

    vps_test_state_init(&state);
    provider = vps_test_provider(&state);
    passed &= vps_expect(vps_allocator_system(&system_allocator) ==
                             VPS_MEMORY_OK &&
                             vps_credential_registry_init(&registry, operations,
                                                          NULL) ==
                                 VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_credential_registry_register(&registry, 7U,
                                                              &provider) ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                         "failure_fixture_init");

    state.mode = VPS_TEST_PROVIDER_FAIL;
    passed &= vps_expect(vps_resolved_credential_init(
                             &resolved, &system_allocator, operations, NULL) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_credential_registry_resolve(
                                 &registry, "missing", 7U, &resolved) ==
                                 VPS_CREDENTIAL_REGISTRY_RESOLVE_FAILED &&
                             state.release_count == 0,
                         "failed_resolve_has_no_release");

    state.mode = VPS_TEST_PROVIDER_PARTIAL;
    state.config.header.present_fields = VPS_CREDENTIAL_FIELD_USER;
    state.config.user = NULL;
    passed &= vps_expect(vps_credential_registry_resolve(
                             &registry, "partial", 7U, &resolved) ==
                             VPS_CREDENTIAL_REGISTRY_INVALID_CONFIG &&
                             state.release_count == 1,
                         "partial_config_released");

    (void)memset(oversize_value, 'x', sizeof(oversize_value));
    oversize_value[sizeof(oversize_value) - 1U] = '\0';
    state.config.header.present_fields = VPS_CREDENTIAL_FIELD_USER;
    state.config.user = oversize_value;
    passed &= vps_expect(vps_credential_registry_resolve(
                             &registry, "oversize", 8U, &resolved) ==
                             VPS_CREDENTIAL_REGISTRY_INVALID_CONFIG &&
                             state.release_count == 2,
                         "oversize_config_released");

    state.mode = VPS_TEST_PROVIDER_SHORT_LEASE;
    passed &= vps_expect(vps_credential_registry_resolve(
                             &registry, "short", 5U, &resolved) ==
                             VPS_CREDENTIAL_REGISTRY_ABI_INCOMPATIBLE &&
                             state.release_count == 3,
                         "short_lease_released");

    state.mode = VPS_TEST_PROVIDER_OLD_CONFIG;
    state.config.header.structure_size =
        (uint32_t)(offsetof(VpsCredentialConfig, password) +
                   sizeof(state.config.password));
    state.config.header.present_fields = VPS_CREDENTIAL_FIELD_USER |
                                         VPS_CREDENTIAL_FIELD_PASSWORD;
    state.config.user = "legacy";
    state.config.password = "old-value";
    passed &= vps_expect(vps_credential_registry_resolve(
                             &registry, "legacy", 6U, &resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             state.release_count == 4 &&
                             strcmp(resolved.config.user, "legacy") == 0,
                         "old_config_prefix_accepted");
    passed &= vps_expect(vps_resolved_credential_cleanup(&resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK,
                         "old_config_cleanup");

    state.mode = VPS_TEST_PROVIDER_LARGE_CONFIG;
    state.config.header.structure_size =
        (uint32_t)(sizeof(state.config) + sizeof(uintptr_t) * 4U);
    state.config.header.present_fields = VPS_CREDENTIAL_FIELD_USER |
                                         (UINT64_C(1) << 61);
    state.config.user = "future";
    state.config.password = NULL;
    passed &= vps_expect(vps_credential_registry_resolve(
                             &registry, "future", 6U, &resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             state.release_count == 5,
                         "larger_config_unknown_fields_accepted");
    passed &= vps_expect(vps_resolved_credential_cleanup(&resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK,
                         "larger_config_cleanup");

    state.config.header.structure_size = (uint32_t)sizeof(state.config);
    state.config.header.present_fields = VPS_CREDENTIAL_FIELD_USER;
    state.config.user = "lease-prefix";
    state.mode = VPS_TEST_PROVIDER_OLD_LEASE;
    passed &= vps_expect(vps_credential_registry_resolve(
                             &registry, "old-lease", 9U, &resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             state.release_count == 6,
                         "old_lease_prefix_accepted");
    passed &= vps_expect(vps_resolved_credential_cleanup(&resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK,
                         "old_lease_cleanup");

    state.mode = VPS_TEST_PROVIDER_LARGE_LEASE;
    passed &= vps_expect(vps_credential_registry_resolve(
                             &registry, "large-lease", 11U, &resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             state.release_count == 7,
                         "larger_lease_unknown_fields_accepted");
    passed &= vps_expect(vps_resolved_credential_cleanup(&resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK,
                         "larger_lease_cleanup");

    passed &= vps_expect(vps_fault_allocator_init(&fault, &system_allocator,
                                                  1U) == VPS_MEMORY_OK &&
                             vps_fault_allocator_make(&fault,
                                                      &fault_allocator) ==
                                 VPS_MEMORY_OK &&
                             vps_resolved_credential_init(
                                 &resolved, &fault_allocator, operations,
                                 NULL) == VPS_CREDENTIAL_REGISTRY_OK,
                         "fault_resolved_init");
    state.config.header.structure_size = (uint32_t)sizeof(state.config);
    state.config.header.present_fields = VPS_CREDENTIAL_FIELD_USER;
    state.config.user = "oom-copy";
    passed &= vps_expect(vps_credential_registry_resolve(
                             &registry, "oom", 3U, &resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OUT_OF_MEMORY &&
                             state.release_count == 8 &&
                             fault.active_allocations == 0U,
                         "copy_oom_still_releases_lease");
    passed &= vps_expect(vps_resolved_credential_cleanup(&resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_credential_registry_cleanup(&registry) ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                         "failure_cleanup");
    return passed;
}

#if defined(_WIN32)
typedef struct VpsResolveThreadState {
    VpsCredentialRegistry *registry;
    VpsCredentialRegistryResult result;
} VpsResolveThreadState;

static DWORD WINAPI vps_resolve_thread(void *context)
{
    VpsResolveThreadState *thread = (VpsResolveThreadState *)context;
    VpsAllocator allocator;
    VpsResolvedCredential resolved = {0};

    if (vps_allocator_system(&allocator) != VPS_MEMORY_OK ||
        vps_resolved_credential_init(&resolved, &allocator,
                                     vps_platform_current_operations(),
                                     NULL) != VPS_CREDENTIAL_REGISTRY_OK) {
        thread->result = VPS_CREDENTIAL_REGISTRY_INVALID_ARGUMENT;
        return 1U;
    }
    thread->result = vps_credential_registry_resolve(
        thread->registry, "concurrent", 10U, &resolved);
    if (thread->result == VPS_CREDENTIAL_REGISTRY_OK &&
        vps_resolved_credential_cleanup(&resolved) !=
            VPS_CREDENTIAL_REGISTRY_OK) {
        thread->result = VPS_CREDENTIAL_REGISTRY_CLEANUP_FAILED;
    }
    return 0U;
}

static int vps_test_concurrent_resolve_and_busy_cleanup(void)
{
    enum { VPS_THREAD_COUNT = 8 };
    const VpsPlatformOperations *operations = vps_platform_current_operations();
    VpsCredentialRegistry registry = {0};
    VpsTestProviderState state;
    VpsCredentialProvider provider;
    VpsResolveThreadState threads[VPS_THREAD_COUNT] = {0};
    HANDLE handles[VPS_THREAD_COUNT] = {0};
    size_t index;
    int passed = 1;

    vps_test_state_init(&state);
    state.config.header.present_fields = VPS_CREDENTIAL_FIELD_USER;
    state.config.user = "thread-user";
    state.config.password = NULL;
    state.entered_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    state.continue_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    state.mode = VPS_TEST_PROVIDER_BLOCK;
    state.block_target = VPS_THREAD_COUNT;
    provider = vps_test_provider(&state);
    passed &= vps_expect(state.entered_event != NULL &&
                             state.continue_event != NULL &&
                             vps_credential_registry_init(&registry, operations,
                                                          NULL) ==
                                 VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_credential_registry_register(&registry, 33U,
                                                              &provider) ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                         "concurrent_fixture_init");
    for (index = 0U; index < VPS_THREAD_COUNT; ++index) {
        threads[index].registry = &registry;
        handles[index] = CreateThread(NULL, 0U, vps_resolve_thread,
                                      &threads[index], 0U, NULL);
        passed &= vps_expect(handles[index] != NULL, "concurrent_thread_create");
    }
    passed &= vps_expect(WaitForSingleObject(state.entered_event, 5000U) ==
                             WAIT_OBJECT_0 &&
                             vps_credential_registry_cleanup(&registry) ==
                                 VPS_CREDENTIAL_REGISTRY_BUSY,
                         "cleanup_busy_during_resolve");
    (void)SetEvent(state.continue_event);
    passed &= vps_expect(WaitForMultipleObjects(VPS_THREAD_COUNT, handles, TRUE,
                                                5000U) == WAIT_OBJECT_0,
                         "concurrent_threads_join");
    for (index = 0U; index < VPS_THREAD_COUNT; ++index) {
        passed &= vps_expect(threads[index].result ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                             "concurrent_resolve_result");
        if (handles[index] != NULL) {
            (void)CloseHandle(handles[index]);
        }
    }
    passed &= vps_expect(state.resolve_count == VPS_THREAD_COUNT &&
                             state.release_count == VPS_THREAD_COUNT &&
                             state.maximum_active > 1 &&
                             registry.active_resolves == 0U,
                         "concurrent_leases_balanced");
    passed &= vps_expect(vps_credential_registry_cleanup(&registry) ==
                             VPS_CREDENTIAL_REGISTRY_OK,
                         "concurrent_registry_cleanup");
    (void)CloseHandle(state.entered_event);
    (void)CloseHandle(state.continue_event);
    return passed;
}
#else
static int vps_test_concurrent_resolve_and_busy_cleanup(void)
{
    return 1;
}
#endif

int main(void)
{
    int passed = 1;

    passed &= vps_test_registration_and_abi();
    passed &= vps_test_resolve_copy_release_and_logging();
    passed &= vps_test_failure_matrix();
    passed &= vps_test_concurrent_resolve_and_busy_cleanup();
    (void)printf("[credential_provider] level=info cases=4 pointer_bits=%zu "
                 "status=%s\n",
                 sizeof(void *) * CHAR_BIT, passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
