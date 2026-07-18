#include "vps_memory.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VpsCountingAllocator {
    size_t allocate_calls;
    size_t reallocate_calls;
    size_t deallocate_calls;
    int reject_reallocate;
} VpsCountingAllocator;

static void *vps_counting_allocate(void *context, size_t size)
{
    VpsCountingAllocator *state = (VpsCountingAllocator *)context;
    state->allocate_calls += 1U;
    return malloc(size);
}

static void *vps_counting_reallocate(void *context,
                                     void *memory,
                                     size_t old_size,
                                     size_t new_size)
{
    VpsCountingAllocator *state = (VpsCountingAllocator *)context;
    (void)old_size;
    state->reallocate_calls += 1U;
    if (state->reject_reallocate) {
        return NULL;
    }
    return realloc(memory, new_size);
}

static void vps_counting_deallocate(void *context,
                                    void *memory,
                                    size_t size)
{
    VpsCountingAllocator *state = (VpsCountingAllocator *)context;
    (void)size;
    state->deallocate_calls += 1U;
    free(memory);
}

static int vps_expect(int condition, const char *case_name)
{
    if (!condition) {
        (void)fprintf(stderr,
                      "[memory] level=error case=%s status=failed\n",
                      case_name);
        return 0;
    }
    return 1;
}

static int vps_test_checked_arithmetic(void)
{
    size_t size_result = 0U;
    uint32_t uint32_result = 0U;
    int int_result = 0;
    int passed = 1;

    passed &= vps_expect(vps_size_add(4U, 5U, &size_result) == VPS_MEMORY_OK &&
                             size_result == 9U,
                         "add_valid");
    passed &= vps_expect(vps_size_add(SIZE_MAX, 1U, &size_result) ==
                             VPS_MEMORY_OVERFLOW,
                         "add_overflow");
    passed &= vps_expect(vps_size_multiply(0U, SIZE_MAX, &size_result) ==
                             VPS_MEMORY_OK &&
                             size_result == 0U,
                         "multiply_zero");
    passed &= vps_expect(vps_size_multiply(SIZE_MAX, 2U, &size_result) ==
                             VPS_MEMORY_OVERFLOW,
                         "multiply_overflow");
    passed &= vps_expect(vps_size_from_int64(-1, &size_result) ==
                             VPS_MEMORY_RANGE_ERROR,
                         "negative_size_rejected");
#if SIZE_MAX < UINT64_MAX
    passed &= vps_expect(vps_size_from_uint64(UINT64_MAX, &size_result) ==
                             VPS_MEMORY_RANGE_ERROR,
                         "uint64_to_size_narrowing_rejected");
#endif
    passed &= vps_expect(vps_size_to_uint32((size_t)UINT32_MAX,
                                            &uint32_result) == VPS_MEMORY_OK &&
                             uint32_result == UINT32_MAX,
                         "uint32_exact_limit");
    passed &= vps_expect(vps_size_to_int((size_t)INT_MAX, &int_result) ==
                             VPS_MEMORY_OK &&
                             int_result == INT_MAX,
                         "int_exact_limit");
#if SIZE_MAX > UINT32_MAX
    passed &= vps_expect(vps_size_to_uint32((size_t)UINT32_MAX + 1U,
                                            &uint32_result) ==
                             VPS_MEMORY_RANGE_ERROR,
                         "uint32_narrowing_rejected");
#endif
    passed &= vps_expect(vps_size_to_int((size_t)INT_MAX + 1U, &int_result) ==
                             VPS_MEMORY_RANGE_ERROR,
                         "int_narrowing_rejected");
    return passed;
}

static int vps_test_allocator_contract(void)
{
    VpsCountingAllocator state = {0U, 0U, 0U, 0};
    VpsAllocator allocator;
    VpsAllocator system_allocator;
    void *memory = NULL;
    void *preserved;
    int passed = 1;

    passed &= vps_expect(
        vps_allocator_init(&allocator, VPS_ALLOCATOR_FAMILY_TEST, &state,
                           vps_counting_allocate, vps_counting_reallocate,
                           vps_counting_deallocate) == VPS_MEMORY_OK,
        "allocator_init");
    passed &= vps_expect(vps_allocator_system(&system_allocator) ==
                             VPS_MEMORY_OK &&
                             system_allocator.family ==
                                 VPS_ALLOCATOR_FAMILY_SYSTEM,
                         "system_allocator_init");
    passed &= vps_expect(vps_allocator_init(
                             &system_allocator, (VpsAllocatorFamily)0, NULL,
                             vps_counting_allocate, vps_counting_reallocate,
                             vps_counting_deallocate) ==
                             VPS_MEMORY_INVALID_ARGUMENT,
                         "invalid_allocator_family_rejected");
    passed &= vps_expect(vps_memory_allocate(&allocator, 0U, &memory) ==
                             VPS_MEMORY_OK &&
                             memory == NULL && state.allocate_calls == 0U,
                         "zero_allocation_no_callback");
    passed &= vps_expect(vps_memory_allocate(&allocator, 4U, &memory) ==
                             VPS_MEMORY_OK &&
                             state.allocate_calls == 1U,
                         "allocation_callback");
    if (memory != NULL) {
        (void)memcpy(memory, "abc", 4U);
    }
    preserved = memory;
    state.reject_reallocate = 1;
    passed &= vps_expect(vps_memory_reallocate(&allocator, &memory, 4U, 8U) ==
                             VPS_MEMORY_OUT_OF_MEMORY &&
                             memory == preserved &&
                             memory != NULL &&
                             memcmp(memory, "abc", 4U) == 0,
                         "failed_resize_preserves_owner");
    state.reject_reallocate = 0;
    passed &= vps_expect(vps_memory_reallocate(&allocator, &memory, 4U, 8U) ==
                             VPS_MEMORY_OK &&
                             state.reallocate_calls == 2U,
                         "resize_callback");
    vps_memory_release(&allocator, &memory, 8U);
    vps_memory_release(&allocator, &memory, 8U);
    passed &= vps_expect(memory == NULL && state.deallocate_calls == 1U,
                         "release_idempotent");
    return passed;
}

static int vps_test_bounded_buffer(void)
{
    static const unsigned char first[] = {1U, 2U, 3U, 4U};
    static const unsigned char second[] = {5U, 6U, 7U, 8U};
    static const unsigned char extra[] = {9U};
    VpsCountingAllocator state = {0U, 0U, 0U, 0};
    VpsAllocator allocator;
    VpsBuffer buffer;
    unsigned char *preserved_data;
    int passed = 1;

    passed &= vps_expect(
        vps_allocator_init(&allocator, VPS_ALLOCATOR_FAMILY_TEST, &state,
                           vps_counting_allocate, vps_counting_reallocate,
                           vps_counting_deallocate) == VPS_MEMORY_OK,
        "buffer_allocator_init");
    passed &= vps_expect(vps_buffer_init(&buffer, &allocator, 8U) ==
                             VPS_MEMORY_OK,
                         "buffer_init");
    passed &= vps_expect(vps_buffer_append(&buffer, NULL, 0U) ==
                             VPS_MEMORY_OK &&
                             buffer.size == 0U,
                         "buffer_zero_append");
    passed &= vps_expect(vps_buffer_append(&buffer, first, sizeof(first)) ==
                             VPS_MEMORY_OK &&
                             buffer.size == sizeof(first),
                         "buffer_first_append");
    preserved_data = buffer.data;
    state.reject_reallocate = 1;
    passed &= vps_expect(vps_buffer_append(&buffer, second, sizeof(second)) ==
                             VPS_MEMORY_OUT_OF_MEMORY &&
                             buffer.data == preserved_data &&
                             buffer.size == sizeof(first) &&
                             memcmp(buffer.data, first, sizeof(first)) == 0,
                         "buffer_failed_grow_preserves_owner");
    state.reject_reallocate = 0;
    passed &= vps_expect(vps_buffer_append(&buffer, second, sizeof(second)) ==
                             VPS_MEMORY_OK &&
                             buffer.size == 8U &&
                             memcmp(buffer.data, first, sizeof(first)) == 0,
                         "buffer_exact_limit");
    passed &= vps_expect(vps_buffer_append(&buffer, extra, sizeof(extra)) ==
                             VPS_MEMORY_LIMIT_EXCEEDED &&
                             buffer.size == 8U,
                         "buffer_limit_rejected");
    passed &= vps_expect(vps_buffer_append(&buffer, NULL, 1U) ==
                             VPS_MEMORY_INVALID_ARGUMENT,
                         "buffer_null_payload_rejected");
    vps_buffer_reset(&buffer);
    vps_buffer_reset(&buffer);
    passed &= vps_expect(buffer.data == NULL && buffer.size == 0U &&
                             buffer.capacity == 0U &&
                             state.deallocate_calls == 1U,
                         "buffer_reset_idempotent");
    return passed;
}

static int vps_test_fault_reallocate(void)
{
    VpsCountingAllocator backing_state = {0U, 0U, 0U, 0};
    VpsAllocator backing;
    VpsAllocator allocator;
    VpsFaultAllocator fault;
    void *memory = NULL;
    void *preserved;
    int passed = 1;

    passed &= vps_expect(
        vps_allocator_init(&backing, VPS_ALLOCATOR_FAMILY_SQLITE,
                           &backing_state, vps_counting_allocate,
                           vps_counting_reallocate,
                           vps_counting_deallocate) == VPS_MEMORY_OK,
        "fault_backing_init");
    passed &= vps_expect(vps_fault_allocator_init(&fault, &backing, 2U) ==
                             VPS_MEMORY_OK &&
                             vps_fault_allocator_make(&fault, &allocator) ==
                                 VPS_MEMORY_OK &&
                             allocator.family == VPS_ALLOCATOR_FAMILY_SQLITE,
                         "fault_family_preserved");
    passed &= vps_expect(vps_memory_allocate(&allocator, 4U, &memory) ==
                             VPS_MEMORY_OK &&
                             fault.attempt_count == 1U &&
                             fault.active_allocations == 1U,
                         "fault_first_attempt_passes");
    preserved = memory;
    passed &= vps_expect(vps_memory_reallocate(&allocator, &memory, 4U, 8U) ==
                             VPS_MEMORY_OUT_OF_MEMORY &&
                             memory == preserved &&
                             fault.attempt_count == 2U &&
                             backing_state.reallocate_calls == 0U,
                         "fault_reallocate_preserves_owner");
    passed &= vps_expect(vps_fault_allocator_reset(&fault, 0U) ==
                             VPS_MEMORY_INVALID_ARGUMENT,
                         "fault_reset_rejects_active_owner");
    vps_memory_release(&allocator, &memory, 4U);
    vps_memory_release(&allocator, &memory, 4U);
    passed &= vps_expect(fault.active_allocations == 0U &&
                             fault.cleanup_errors == 0U &&
                             backing_state.deallocate_calls == 1U,
                         "fault_cleanup_exactly_once");
    passed &= vps_expect(vps_fault_allocator_reset(&fault, 0U) ==
                             VPS_MEMORY_OK &&
                             fault.attempt_count == 0U,
                         "fault_reset_after_cleanup");
    return passed;
}

static int vps_run_partial_fixture(size_t fail_at_attempt)
{
    VpsCountingAllocator backing_state = {0U, 0U, 0U, 0};
    VpsAllocator backing;
    VpsAllocator allocator;
    VpsFaultAllocator fault;
    VpsOwnedMemory first = {0};
    VpsOwnedMemory second = {0};
    VpsMemoryResult first_result;
    VpsMemoryResult second_result = VPS_MEMORY_OUT_OF_MEMORY;
    int passed = 1;

    passed &= vps_expect(
        vps_allocator_init(&backing, VPS_ALLOCATOR_FAMILY_TEST,
                           &backing_state, vps_counting_allocate,
                           vps_counting_reallocate,
                           vps_counting_deallocate) == VPS_MEMORY_OK,
        "partial_backing_init");
    passed &= vps_expect(vps_fault_allocator_init(&fault, &backing,
                                                  fail_at_attempt) ==
                             VPS_MEMORY_OK &&
                             vps_fault_allocator_make(&fault, &allocator) ==
                                 VPS_MEMORY_OK,
                         "partial_fault_init");
    passed &= vps_expect(vps_owned_memory_init(&first, &allocator) ==
                             VPS_MEMORY_OK &&
                             vps_owned_memory_init(&second, &allocator) ==
                                 VPS_MEMORY_OK,
                         "partial_owner_init");

    first_result = vps_owned_memory_allocate(&first, 8U);
    if (first_result == VPS_MEMORY_OK) {
        second_result = vps_owned_memory_allocate(&second, 16U);
    }
    if (fail_at_attempt == 1U) {
        passed &= vps_expect(first_result == VPS_MEMORY_OUT_OF_MEMORY &&
                                 !first.owned && !second.owned,
                             "partial_fail_before_first");
    } else if (fail_at_attempt == 2U) {
        passed &= vps_expect(first_result == VPS_MEMORY_OK && first.owned &&
                                 second_result == VPS_MEMORY_OUT_OF_MEMORY &&
                                 !second.owned,
                             "partial_fail_after_first");
    } else {
        passed &= vps_expect(first_result == VPS_MEMORY_OK && first.owned &&
                                 second_result == VPS_MEMORY_OK && second.owned,
                             "partial_no_failure");
    }

    vps_owned_memory_release(&second);
    vps_owned_memory_release(&first);
    vps_owned_memory_release(&second);
    vps_owned_memory_release(&first);
    passed &= vps_expect(fault.active_allocations == 0U &&
                             fault.cleanup_errors == 0U &&
                             backing_state.allocate_calls ==
                                 backing_state.deallocate_calls,
                         "partial_cleanup_balanced");
    (void)printf("[memory] level=info case=partial_cleanup "
                 "fault_attempt=%zu resource=owned_memory cleanup=%s\n",
                 fail_at_attempt, passed ? "balanced" : "failed");
    return passed;
}

static int vps_test_fault_partial_cleanup(void)
{
    int passed = 1;

    passed &= vps_run_partial_fixture(1U);
    passed &= vps_run_partial_fixture(2U);
    passed &= vps_run_partial_fixture(3U);
    return passed;
}

static int vps_test_ownership_transfer(void)
{
    VpsCountingAllocator backing_state = {0U, 0U, 0U, 0};
    VpsAllocator allocator;
    VpsOwnedMemory owner = {0};
    void *memory = NULL;
    size_t size = 0U;
    int passed = 1;

    passed &= vps_expect(
        vps_allocator_init(&allocator, VPS_ALLOCATOR_FAMILY_TEST,
                           &backing_state, vps_counting_allocate,
                           vps_counting_reallocate,
                           vps_counting_deallocate) == VPS_MEMORY_OK &&
            vps_owned_memory_init(&owner, &allocator) == VPS_MEMORY_OK,
        "transfer_owner_init");
    passed &= vps_expect(vps_owned_memory_allocate(&owner, 8U) ==
                             VPS_MEMORY_OK &&
                             vps_owned_memory_detach(&owner, &memory, &size) ==
                                 VPS_MEMORY_OK &&
                             !owner.owned && owner.memory == NULL && size == 8U,
                         "transfer_detach");
    passed &= vps_expect(vps_owned_memory_adopt(&owner, memory, size) ==
                             VPS_MEMORY_OK &&
                             owner.owned,
                         "transfer_adopt");
    memory = NULL;
    size = 0U;
    vps_owned_memory_release(&owner);
    vps_owned_memory_release(&owner);
    passed &= vps_expect(backing_state.allocate_calls == 1U &&
                             backing_state.deallocate_calls == 1U,
                         "transfer_cleanup_exactly_once");
    return passed;
}

int main(void)
{
    int passed = 1;

    passed &= vps_test_checked_arithmetic();
    passed &= vps_test_allocator_contract();
    passed &= vps_test_bounded_buffer();
    passed &= vps_test_fault_reallocate();
    passed &= vps_test_fault_partial_cleanup();
    passed &= vps_test_ownership_transfer();
    (void)printf("[memory] level=info cases=6 pointer_bits=%zu status=%s\n",
                 sizeof(void *) * CHAR_BIT, passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
