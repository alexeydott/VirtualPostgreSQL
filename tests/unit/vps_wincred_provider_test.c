#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "vps_wincred_provider.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum VpsFakeReadMode {
    VPS_FAKE_READ_FOUND = 0,
    VPS_FAKE_READ_MISSING = 1,
    VPS_FAKE_READ_ERROR = 2
} VpsFakeReadMode;

static const uint16_t vps_ascii_target[] = {
    'f', 'o', 'r', 'b', 'i', 'd', 'd', 'e', 'n', '!', 't', 'a', 'r', 'g',
    'e', 't'};
static const uint16_t vps_unicode_target[] = {
    UINT16_C(0x0446), UINT16_C(0x0435), UINT16_C(0x043b), UINT16_C(0x044c)};
static const char vps_unicode_target_utf8[] =
    "\xd1\x86\xd0\xb5\xd0\xbb\xd1\x8c";

typedef struct VpsFakeWinCredState {
    unsigned char blob[VPS_WINCRED_BLOB_MAX_LENGTH + 1U];
    size_t blob_size;
    VpsFakeReadMode mode;
    volatile long read_count;
    volatile long release_count;
    volatile long active_reads;
    volatile long maximum_active_reads;
    volatile long nonzero_blob_release;
    volatile long target_mismatch;
    const uint16_t *expected_target;
    size_t expected_target_length;
    long block_target;
    HANDLE entered_event;
    HANDLE continue_event;
} VpsFakeWinCredState;

typedef struct VpsWipeAllocatorState {
    size_t deallocate_count;
    int nonzero_byte_seen;
} VpsWipeAllocatorState;

typedef struct VpsWinCredLogState {
    size_t event_count;
    int forbidden_value_seen;
} VpsWinCredLogState;

static void vps_write_u16(unsigned char *output, uint16_t value)
{
    output[0] = (unsigned char)(value & UINT16_C(0xff));
    output[1] = (unsigned char)(value >> 8);
}

static void vps_write_u32(unsigned char *output, uint32_t value)
{
    output[0] = (unsigned char)(value & UINT32_C(0xff));
    output[1] = (unsigned char)((value >> 8) & UINT32_C(0xff));
    output[2] = (unsigned char)((value >> 16) & UINT32_C(0xff));
    output[3] = (unsigned char)(value >> 24);
}

static void vps_write_u64(unsigned char *output, uint64_t value)
{
    vps_write_u32(output, (uint32_t)(value & UINT64_C(0xffffffff)));
    vps_write_u32(output + 4U, (uint32_t)(value >> 32));
}

static void vps_blob_begin(VpsFakeWinCredState *state, uint16_t field_count)
{
    (void)memset(state->blob, 0, sizeof(state->blob));
    vps_write_u32(state->blob, VPS_WINCRED_FORMAT_MAGIC);
    vps_write_u16(state->blob + 4U, VPS_WINCRED_FORMAT_VERSION);
    vps_write_u16(state->blob + 6U, field_count);
    state->blob_size = VPS_WINCRED_FORMAT_HEADER_SIZE;
}

static int vps_blob_add(VpsFakeWinCredState *state,
                        VpsCredentialFields field,
                        const unsigned char *value,
                        size_t value_size)
{
    size_t required = VPS_WINCRED_FORMAT_ENTRY_SIZE + value_size;
    unsigned char *entry;

    if (required > sizeof(state->blob) - state->blob_size ||
        value_size > UINT32_MAX) {
        return 0;
    }
    entry = state->blob + state->blob_size;
    vps_write_u64(entry, field);
    vps_write_u32(entry + 8U, (uint32_t)value_size);
    vps_write_u32(entry + 12U, 0U);
    if (value_size != 0U) {
        (void)memcpy(entry + VPS_WINCRED_FORMAT_ENTRY_SIZE, value, value_size);
    }
    state->blob_size += required;
    return 1;
}

static int vps_blob_add_text(VpsFakeWinCredState *state,
                             VpsCredentialFields field,
                             const char *value)
{
    return vps_blob_add(state, field, (const unsigned char *)value,
                        strlen(value));
}

static void vps_fake_state_valid(VpsFakeWinCredState *state)
{
    (void)memset(state, 0, sizeof(*state));
    vps_blob_begin(state, 4U);
    (void)vps_blob_add_text(state, VPS_CREDENTIAL_FIELD_HOSTS,
                            "db.internal");
    (void)vps_blob_add_text(state, VPS_CREDENTIAL_FIELD_USER, "readonly");
    (void)vps_blob_add_text(state, VPS_CREDENTIAL_FIELD_PASSWORD,
                            "opaque-value");
    (void)vps_blob_add_text(state, VPS_CREDENTIAL_FIELD_SSLMODE,
                            "verify-full");
    state->expected_target = vps_ascii_target;
    state->expected_target_length =
        sizeof(vps_ascii_target) / sizeof(vps_ascii_target[0]);
}

static void vps_update_maximum(volatile long *maximum, long value)
{
    long observed = *maximum;
    while (value > observed) {
        long previous = InterlockedCompareExchange(maximum, value, observed);
        if (previous == observed) {
            break;
        }
        observed = previous;
    }
}

static VpsWinCredReadResult vps_fake_read(void *context,
                                          const uint16_t *target,
                                          size_t target_length,
                                          VpsWinCredRecord *record)
{
    VpsFakeWinCredState *state = (VpsFakeWinCredState *)context;
    long active;
    unsigned char *copy;

    (void)InterlockedIncrement(&state->read_count);
    if (target_length != state->expected_target_length ||
        memcmp(target, state->expected_target,
               state->expected_target_length * sizeof(uint16_t)) != 0) {
        (void)InterlockedIncrement(&state->target_mismatch);
    }
    if (state->mode == VPS_FAKE_READ_MISSING) {
        return VPS_WINCRED_READ_NOT_FOUND;
    }
    if (state->mode == VPS_FAKE_READ_ERROR) {
        return VPS_WINCRED_READ_ERROR;
    }
    active = InterlockedIncrement(&state->active_reads);
    vps_update_maximum(&state->maximum_active_reads, active);
    if (state->block_target != 0) {
        if (active >= state->block_target) {
            (void)SetEvent(state->entered_event);
        }
        (void)WaitForSingleObject(state->continue_event, 5000U);
    }
    copy = (unsigned char *)malloc(state->blob_size == 0U ? 1U
                                                          : state->blob_size);
    if (copy == NULL) {
        (void)InterlockedDecrement(&state->active_reads);
        return VPS_WINCRED_READ_ERROR;
    }
    if (state->blob_size != 0U) {
        (void)memcpy(copy, state->blob, state->blob_size);
    }
    record->blob = state->blob_size == 0U ? NULL : copy;
    record->blob_size = state->blob_size;
    record->native_record = copy;
    (void)InterlockedDecrement(&state->active_reads);
    return VPS_WINCRED_READ_OK;
}

static void vps_fake_release(void *context, VpsWinCredRecord *record)
{
    VpsFakeWinCredState *state = (VpsFakeWinCredState *)context;
    size_t index;

    for (index = 0U; index < record->blob_size; ++index) {
        if (record->blob[index] != 0U) {
            (void)InterlockedIncrement(&state->nonzero_blob_release);
            break;
        }
    }
    free(record->native_record);
    (void)memset(record, 0, sizeof(*record));
    (void)InterlockedIncrement(&state->release_count);
}

static const VpsWinCredApi vps_fake_api = {
    vps_fake_read, vps_fake_release};

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

static int vps_log_sink(void *context, const VpsLogEvent *event)
{
    VpsWinCredLogState *state = (VpsWinCredLogState *)context;
    size_t index;

    state->event_count += 1U;
    for (index = 0U; index < event->field_count; ++index) {
        const VpsLogField *field = &event->fields[index];
        if (field->type == VPS_LOG_FIELD_TYPE_STRING &&
            (memchr(field->value.string_value.data, '!',
                    field->value.string_value.length) != NULL ||
             memchr(field->value.string_value.data, '\\',
                    field->value.string_value.length) != NULL)) {
            state->forbidden_value_seen = 1;
        }
    }
    return 0;
}

static int vps_expect(int condition, const char *case_name)
{
    if (!condition) {
        (void)fprintf(stderr,
                      "[wincred] level=error case=%s status=failed\n",
                      case_name);
        return 0;
    }
    return 1;
}

static int vps_test_found_registry_and_zero(void)
{
    const VpsPlatformOperations *operations = vps_platform_current_operations();
    VpsFakeWinCredState fake;
    VpsWipeAllocatorState wipe = {0};
    VpsWinCredLogState log_state = {0};
    VpsAllocator allocator;
    VpsAllocator resolved_allocator;
    VpsLogger logger;
    VpsWinCredProviderContext context = {0};
    VpsCredentialProvider provider;
    VpsCredentialRegistry registry = {0};
    VpsResolvedCredential resolved = {0};
    int passed = 1;

    vps_fake_state_valid(&fake);
    passed &= vps_expect(
        vps_allocator_init(&allocator, VPS_ALLOCATOR_FAMILY_TEST, &wipe,
                           vps_wipe_allocate, vps_wipe_reallocate,
                           vps_wipe_deallocate) == VPS_MEMORY_OK &&
            vps_allocator_system(&resolved_allocator) == VPS_MEMORY_OK &&
            vps_logger_init(&logger, VPS_LOG_LEVEL_DEBUG, vps_log_sink,
                            &log_state) == VPS_LOG_OK &&
            vps_wincred_provider_init_with_api(
                &context, &allocator, operations, &logger, &vps_fake_api,
                &fake) == VPS_CREDENTIAL_REGISTRY_OK &&
            vps_wincred_provider_make(&context, &provider) ==
                VPS_CREDENTIAL_REGISTRY_OK &&
            vps_credential_registry_init(&registry, operations, &logger) ==
                VPS_CREDENTIAL_REGISTRY_OK &&
            vps_credential_registry_register(&registry, 27U, &provider) ==
                VPS_CREDENTIAL_REGISTRY_OK &&
            vps_resolved_credential_init(&resolved, &resolved_allocator,
                                         operations, NULL) ==
                VPS_CREDENTIAL_REGISTRY_OK,
        "found_fixture_init");
    passed &= vps_expect(
        vps_credential_registry_resolve(&registry, "forbidden!target", 16U,
                                        &resolved) ==
                VPS_CREDENTIAL_REGISTRY_OK &&
            strcmp(resolved.config.hosts, "db.internal") == 0 &&
            strcmp(resolved.config.user, "readonly") == 0 &&
            strcmp(resolved.config.password, "opaque-value") == 0 &&
            strcmp(resolved.config.sslmode, "verify-full") == 0,
        "found_structured_decode");
    passed &= vps_expect(fake.read_count == 1 && fake.release_count == 1 &&
                             fake.nonzero_blob_release == 0 &&
                             fake.target_mismatch == 0,
                         "native_record_zeroed_and_released_once");
    passed &= vps_expect(log_state.event_count != 0U &&
                             !log_state.forbidden_value_seen,
                         "logs_exclude_target_and_values");
    passed &= vps_expect(vps_resolved_credential_cleanup(&resolved) ==
                             VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_credential_registry_cleanup(&registry) ==
                                 VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_wincred_provider_cleanup(&context) ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                         "found_cleanup");
    passed &= vps_expect(vps_wincred_provider_cleanup(&context) ==
                             VPS_CREDENTIAL_REGISTRY_OK,
                         "found_cleanup_repeat");
    passed &= vps_expect(wipe.deallocate_count == 3U &&
                             !wipe.nonzero_byte_seen,
                         "provider_allocations_zero_before_free");
    return passed;
}

static int32_t vps_direct_resolve(VpsCredentialProvider *provider,
                                  const char *reference,
                                  uint32_t reference_length,
                                  int release_twice)
{
    VpsCredentialLease lease = {0};
    int32_t result = provider->resolve(provider->provider_context, reference,
                                       reference_length, &lease);
    if (result == VPS_CREDENTIAL_PROVIDER_OK) {
        provider->release(provider->provider_context, &lease);
        if (release_twice) {
            provider->release(provider->provider_context, &lease);
        }
    }
    return result;
}

static int vps_test_missing_malformed_and_limits(void)
{
    const VpsPlatformOperations *operations = vps_platform_current_operations();
    VpsFakeWinCredState fake;
    VpsAllocator allocator;
    VpsWinCredProviderContext context = {0};
    VpsCredentialProvider provider;
    char oversize_target[VPS_CREDENTIAL_REFERENCE_MAX_LENGTH + 1U];
    static const unsigned char invalid_utf8[] = {0xc0U, 0xafU};
    VpsCredentialLease lease = {0};
    int passed = 1;

    vps_fake_state_valid(&fake);
    passed &= vps_expect(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                             vps_wincred_provider_init_with_api(
                                 &context, &allocator, operations, NULL,
                                 &vps_fake_api, &fake) ==
                                 VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_wincred_provider_make(&context, &provider) ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                         "negative_fixture_init");
    fake.mode = VPS_FAKE_READ_MISSING;
    passed &= vps_expect(vps_direct_resolve(&provider, "forbidden!target", 16U,
                                            0) ==
                             VPS_CREDENTIAL_PROVIDER_NOT_FOUND &&
                             fake.release_count == 0,
                         "missing_mapping");
    fake.mode = VPS_FAKE_READ_ERROR;
    passed &= vps_expect(vps_direct_resolve(&provider, "forbidden!target", 16U,
                                            0) ==
                             VPS_CREDENTIAL_PROVIDER_ERROR &&
                             fake.release_count == 0,
                         "read_error_mapping");
    fake.mode = VPS_FAKE_READ_FOUND;

    vps_blob_begin(&fake, 1U);
    fake.blob[0] ^= 1U;
    passed &= vps_expect(vps_direct_resolve(&provider, "forbidden!target", 16U,
                                            0) ==
                             VPS_CREDENTIAL_PROVIDER_ERROR &&
                             fake.release_count == 1,
                         "bad_magic_released");
    vps_blob_begin(&fake, 1U);
    (void)vps_blob_add_text(&fake, VPS_CREDENTIAL_FIELD_USER, "first");
    vps_write_u16(fake.blob + 6U, 2U);
    (void)vps_blob_add_text(&fake, VPS_CREDENTIAL_FIELD_USER, "second");
    passed &= vps_expect(vps_direct_resolve(&provider, "forbidden!target", 16U,
                                            0) ==
                             VPS_CREDENTIAL_PROVIDER_ERROR &&
                             fake.release_count == 2,
                         "duplicate_field_released");
    vps_blob_begin(&fake, 1U);
    (void)vps_blob_add(&fake, VPS_CREDENTIAL_FIELD_USER, invalid_utf8,
                       sizeof(invalid_utf8));
    passed &= vps_expect(vps_direct_resolve(&provider, "forbidden!target", 16U,
                                            0) ==
                             VPS_CREDENTIAL_PROVIDER_ERROR &&
                             fake.release_count == 3,
                         "invalid_blob_utf8_released");
    vps_blob_begin(&fake, 1U);
    (void)vps_blob_add_text(&fake, UINT64_C(1) << 63, "future");
    passed &= vps_expect(vps_direct_resolve(&provider, "forbidden!target", 16U,
                                            0) ==
                             VPS_CREDENTIAL_PROVIDER_ERROR &&
                             fake.release_count == 4,
                         "unknown_field_released");
    (void)memset(fake.blob, 'x', sizeof(fake.blob));
    fake.blob_size = sizeof(fake.blob);
    passed &= vps_expect(vps_direct_resolve(&provider, "forbidden!target", 16U,
                                            0) ==
                             VPS_CREDENTIAL_PROVIDER_ERROR &&
                             fake.release_count == 5,
                         "oversize_blob_released");

    (void)memset(oversize_target, 'a', sizeof(oversize_target));
    passed &= vps_expect(vps_direct_resolve(
                             &provider, oversize_target,
                             (uint32_t)sizeof(oversize_target), 0) ==
                             VPS_CREDENTIAL_PROVIDER_INVALID_REFERENCE &&
                             fake.read_count == 7,
                         "oversize_target_rejected_before_read");
    passed &= vps_expect(vps_direct_resolve(
                             &provider, (const char *)invalid_utf8,
                             (uint32_t)sizeof(invalid_utf8), 0) ==
                             VPS_CREDENTIAL_PROVIDER_INVALID_REFERENCE &&
                             fake.read_count == 7,
                         "invalid_target_utf8_rejected_before_read");

    vps_fake_state_valid(&fake);
    fake.expected_target = vps_unicode_target;
    fake.expected_target_length =
        sizeof(vps_unicode_target) / sizeof(vps_unicode_target[0]);
    passed &= vps_expect(provider.resolve(provider.provider_context,
                                          vps_unicode_target_utf8,
                                          (uint32_t)(sizeof(
                                              vps_unicode_target_utf8) - 1U),
                                          &lease) ==
                             VPS_CREDENTIAL_PROVIDER_OK &&
                             context.active_leases == 1U &&
                             fake.target_mismatch == 0 &&
                             vps_wincred_provider_cleanup(&context) ==
                                 VPS_CREDENTIAL_REGISTRY_BUSY,
                         "cleanup_busy_with_outstanding_lease");
    provider.release(provider.provider_context, &lease);
    provider.release(provider.provider_context, &lease);
    passed &= vps_expect(fake.release_count == 1 &&
                             context.active_leases == 0U &&
                             vps_wincred_provider_cleanup(&context) ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                         "provider_release_repeat_safe");
    return passed;
}

static int vps_test_allocation_faults(void)
{
    const VpsPlatformOperations *operations = vps_platform_current_operations();
    VpsFakeWinCredState fake;
    VpsAllocator system_allocator;
    VpsAllocator allocator;
    VpsFaultAllocator fault;
    VpsWinCredProviderContext context = {0};
    VpsCredentialProvider provider;
    size_t fail_at;
    int passed = 1;

    vps_fake_state_valid(&fake);
    passed &= vps_expect(vps_allocator_system(&system_allocator) ==
                             VPS_MEMORY_OK &&
                             vps_fault_allocator_init(&fault,
                                                      &system_allocator, 1U) ==
                                 VPS_MEMORY_OK &&
                             vps_fault_allocator_make(&fault, &allocator) ==
                                 VPS_MEMORY_OK &&
                             vps_wincred_provider_init_with_api(
                                 &context, &allocator, operations, NULL,
                                 &vps_fake_api, &fake) ==
                                 VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_wincred_provider_make(&context, &provider) ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                         "fault_fixture_init");
    for (fail_at = 1U; fail_at <= 3U; ++fail_at) {
        long releases_before = fake.release_count;
        int32_t result;

        passed &= vps_expect(vps_fault_allocator_reset(&fault, fail_at) ==
                                 VPS_MEMORY_OK,
                             "fault_reset");
        result = vps_direct_resolve(&provider, "forbidden!target", 16U, 0);
        passed &= vps_expect(result == VPS_CREDENTIAL_PROVIDER_ERROR &&
                                 fault.active_allocations == 0U &&
                                 fault.cleanup_errors == 0U,
                             "fault_cleanup_balanced");
        if (fail_at == 1U) {
            passed &= vps_expect(fake.release_count == releases_before,
                                 "target_oom_before_read");
        } else {
            passed &= vps_expect(fake.release_count == releases_before + 1,
                                 "decode_oom_releases_native");
        }
    }
    passed &= vps_expect(vps_wincred_provider_cleanup(&context) ==
                             VPS_CREDENTIAL_REGISTRY_OK,
                         "fault_context_cleanup");
    return passed;
}

typedef struct VpsWinCredThreadState {
    VpsCredentialProvider *provider;
    int32_t result;
} VpsWinCredThreadState;

static DWORD WINAPI vps_wincred_thread(void *thread_context)
{
    VpsWinCredThreadState *thread =
        (VpsWinCredThreadState *)thread_context;
    thread->result = vps_direct_resolve(thread->provider, "forbidden!target",
                                        16U, 0);
    return 0U;
}

static int vps_test_concurrent_reads(void)
{
    enum { VPS_THREAD_COUNT = 8 };
    const VpsPlatformOperations *operations = vps_platform_current_operations();
    VpsFakeWinCredState fake;
    VpsAllocator allocator;
    VpsWinCredProviderContext context = {0};
    VpsCredentialProvider provider;
    VpsWinCredThreadState threads[VPS_THREAD_COUNT] = {0};
    HANDLE handles[VPS_THREAD_COUNT] = {0};
    size_t index;
    int passed = 1;

    vps_fake_state_valid(&fake);
    fake.block_target = VPS_THREAD_COUNT;
    fake.entered_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    fake.continue_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    passed &= vps_expect(fake.entered_event != NULL &&
                             fake.continue_event != NULL &&
                             vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
                             vps_wincred_provider_init_with_api(
                                 &context, &allocator, operations, NULL,
                                 &vps_fake_api, &fake) ==
                                 VPS_CREDENTIAL_REGISTRY_OK &&
                             vps_wincred_provider_make(&context, &provider) ==
                                 VPS_CREDENTIAL_REGISTRY_OK,
                         "concurrent_fixture_init");
    for (index = 0U; index < VPS_THREAD_COUNT; ++index) {
        threads[index].provider = &provider;
        handles[index] = CreateThread(NULL, 0U, vps_wincred_thread,
                                      &threads[index], 0U, NULL);
        passed &= vps_expect(handles[index] != NULL, "thread_create");
    }
    passed &= vps_expect(WaitForSingleObject(fake.entered_event, 5000U) ==
                             WAIT_OBJECT_0 &&
                             vps_wincred_provider_cleanup(&context) ==
                                 VPS_CREDENTIAL_REGISTRY_BUSY,
                         "cleanup_busy_during_reads");
    (void)SetEvent(fake.continue_event);
    passed &= vps_expect(WaitForMultipleObjects(VPS_THREAD_COUNT, handles, TRUE,
                                                5000U) == WAIT_OBJECT_0,
                         "thread_join");
    for (index = 0U; index < VPS_THREAD_COUNT; ++index) {
        passed &= vps_expect(threads[index].result ==
                                 VPS_CREDENTIAL_PROVIDER_OK,
                             "concurrent_result");
        if (handles[index] != NULL) {
            (void)CloseHandle(handles[index]);
        }
    }
    passed &= vps_expect(fake.read_count == VPS_THREAD_COUNT &&
                             fake.release_count == VPS_THREAD_COUNT &&
                             fake.maximum_active_reads == VPS_THREAD_COUNT &&
                             fake.nonzero_blob_release == 0 &&
                             context.active_resolves == 0U &&
                             context.active_leases == 0U,
                         "concurrent_reads_balanced");
    passed &= vps_expect(vps_wincred_provider_cleanup(&context) ==
                             VPS_CREDENTIAL_REGISTRY_OK,
                         "concurrent_cleanup");
    (void)CloseHandle(fake.entered_event);
    (void)CloseHandle(fake.continue_event);
    return passed;
}

int main(void)
{
    int passed = 1;

    passed &= vps_test_found_registry_and_zero();
    passed &= vps_test_missing_malformed_and_limits();
    passed &= vps_test_allocation_faults();
    passed &= vps_test_concurrent_reads();
    (void)printf("[wincred] level=info cases=4 pointer_bits=%zu status=%s\n",
                 sizeof(void *) * CHAR_BIT, passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
