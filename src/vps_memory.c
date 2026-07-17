#include "vps_memory.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void *vps_system_allocate(void *context, size_t size)
{
    (void)context;
    return malloc(size);
}

static void *vps_system_reallocate(void *context,
                                   void *memory,
                                   size_t old_size,
                                   size_t new_size)
{
    (void)context;
    (void)old_size;
    return realloc(memory, new_size);
}

static void vps_system_deallocate(void *context, void *memory, size_t size)
{
    (void)context;
    (void)size;
    free(memory);
}

static int vps_fault_allocator_should_fail(VpsFaultAllocator *fault)
{
    fault->attempt_count += 1U;
    return fault->fail_at_attempt != 0U &&
           fault->attempt_count == fault->fail_at_attempt;
}

static void *vps_fault_allocate(void *context, size_t size)
{
    VpsFaultAllocator *fault = (VpsFaultAllocator *)context;
    void *memory;

    if (vps_fault_allocator_should_fail(fault)) {
        return NULL;
    }
    memory = fault->backing.allocate(fault->backing.context, size);
    if (memory != NULL) {
        fault->active_allocations += 1U;
    }
    return memory;
}

static void *vps_fault_reallocate(void *context,
                                  void *memory,
                                  size_t old_size,
                                  size_t new_size)
{
    VpsFaultAllocator *fault = (VpsFaultAllocator *)context;

    if (vps_fault_allocator_should_fail(fault)) {
        return NULL;
    }
    return fault->backing.reallocate(fault->backing.context, memory, old_size,
                                     new_size);
}

static void vps_fault_deallocate(void *context, void *memory, size_t size)
{
    VpsFaultAllocator *fault = (VpsFaultAllocator *)context;

    fault->backing.deallocate(fault->backing.context, memory, size);
    if (fault->active_allocations == 0U) {
        fault->cleanup_errors += 1U;
        return;
    }
    fault->active_allocations -= 1U;
}

VpsMemoryResult vps_allocator_init(VpsAllocator *allocator,
                                   VpsAllocatorFamily family,
                                   void *context,
                                   VpsAllocateFunction allocate,
                                   VpsReallocateFunction reallocate,
                                   VpsDeallocateFunction deallocate)
{
    if (allocator == NULL || allocate == NULL || reallocate == NULL ||
        deallocate == NULL) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    if (family != VPS_ALLOCATOR_FAMILY_SYSTEM &&
        family != VPS_ALLOCATOR_FAMILY_SQLITE &&
        family != VPS_ALLOCATOR_FAMILY_TEST) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }

    allocator->family = family;
    allocator->context = context;
    allocator->allocate = allocate;
    allocator->reallocate = reallocate;
    allocator->deallocate = deallocate;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_allocator_system(VpsAllocator *allocator)
{
    return vps_allocator_init(allocator, VPS_ALLOCATOR_FAMILY_SYSTEM, NULL,
                              vps_system_allocate, vps_system_reallocate,
                              vps_system_deallocate);
}

int vps_allocator_is_valid(const VpsAllocator *allocator)
{
    if (allocator == NULL || allocator->allocate == NULL ||
        allocator->reallocate == NULL || allocator->deallocate == NULL) {
        return 0;
    }
    return allocator->family == VPS_ALLOCATOR_FAMILY_SYSTEM ||
           allocator->family == VPS_ALLOCATOR_FAMILY_SQLITE ||
           allocator->family == VPS_ALLOCATOR_FAMILY_TEST;
}

VpsMemoryResult vps_memory_allocate(const VpsAllocator *allocator,
                                    size_t size,
                                    void **memory)
{
    void *allocated;

    if (memory == NULL || !vps_allocator_is_valid(allocator)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    *memory = NULL;
    if (size == 0U) {
        return VPS_MEMORY_OK;
    }

    allocated = allocator->allocate(allocator->context, size);
    if (allocated == NULL) {
        return VPS_MEMORY_OUT_OF_MEMORY;
    }
    *memory = allocated;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_memory_reallocate(const VpsAllocator *allocator,
                                      void **memory,
                                      size_t old_size,
                                      size_t new_size)
{
    void *resized;

    if (memory == NULL || !vps_allocator_is_valid(allocator)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    if (new_size == 0U) {
        vps_memory_release(allocator, memory, old_size);
        return VPS_MEMORY_OK;
    }
    if (*memory == NULL) {
        return vps_memory_allocate(allocator, new_size, memory);
    }

    resized = allocator->reallocate(allocator->context, *memory, old_size,
                                    new_size);
    if (resized == NULL) {
        return VPS_MEMORY_OUT_OF_MEMORY;
    }
    *memory = resized;
    return VPS_MEMORY_OK;
}

void vps_memory_release(const VpsAllocator *allocator,
                        void **memory,
                        size_t size)
{
    if (memory == NULL || *memory == NULL ||
        !vps_allocator_is_valid(allocator)) {
        return;
    }
    allocator->deallocate(allocator->context, *memory, size);
    *memory = NULL;
}

VpsMemoryResult vps_fault_allocator_init(VpsFaultAllocator *fault,
                                         const VpsAllocator *backing,
                                         size_t fail_at_attempt)
{
    if (fault == NULL || !vps_allocator_is_valid(backing)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    fault->backing = *backing;
    fault->fail_at_attempt = fail_at_attempt;
    fault->attempt_count = 0U;
    fault->active_allocations = 0U;
    fault->cleanup_errors = 0U;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_fault_allocator_make(VpsFaultAllocator *fault,
                                         VpsAllocator *allocator)
{
    if (fault == NULL || allocator == NULL ||
        !vps_allocator_is_valid(&fault->backing)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    return vps_allocator_init(allocator, fault->backing.family, fault,
                              vps_fault_allocate, vps_fault_reallocate,
                              vps_fault_deallocate);
}

VpsMemoryResult vps_fault_allocator_reset(VpsFaultAllocator *fault,
                                          size_t fail_at_attempt)
{
    if (fault == NULL || !vps_allocator_is_valid(&fault->backing) ||
        fault->active_allocations != 0U) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    fault->fail_at_attempt = fail_at_attempt;
    fault->attempt_count = 0U;
    fault->cleanup_errors = 0U;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_owned_memory_init(VpsOwnedMemory *owned_memory,
                                      const VpsAllocator *allocator)
{
    if (owned_memory == NULL || !vps_allocator_is_valid(allocator)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    owned_memory->allocator = *allocator;
    owned_memory->memory = NULL;
    owned_memory->size = 0U;
    owned_memory->owned = 0;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_owned_memory_allocate(VpsOwnedMemory *owned_memory,
                                          size_t size)
{
    void *memory = NULL;
    VpsMemoryResult result;

    if (owned_memory == NULL || owned_memory->owned ||
        owned_memory->memory != NULL ||
        !vps_allocator_is_valid(&owned_memory->allocator)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    result = vps_memory_allocate(&owned_memory->allocator, size, &memory);
    if (result != VPS_MEMORY_OK) {
        return result;
    }
    owned_memory->memory = memory;
    owned_memory->size = size;
    owned_memory->owned = memory != NULL;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_owned_memory_adopt(VpsOwnedMemory *owned_memory,
                                       void *memory,
                                       size_t size)
{
    if (owned_memory == NULL || owned_memory->owned ||
        owned_memory->memory != NULL ||
        !vps_allocator_is_valid(&owned_memory->allocator) ||
        ((memory == NULL) != (size == 0U))) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    owned_memory->memory = memory;
    owned_memory->size = size;
    owned_memory->owned = memory != NULL;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_owned_memory_detach(VpsOwnedMemory *owned_memory,
                                        void **memory,
                                        size_t *size)
{
    if (owned_memory == NULL || memory == NULL || size == NULL ||
        !vps_allocator_is_valid(&owned_memory->allocator)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    *memory = owned_memory->memory;
    *size = owned_memory->size;
    owned_memory->memory = NULL;
    owned_memory->size = 0U;
    owned_memory->owned = 0;
    return VPS_MEMORY_OK;
}

void vps_owned_memory_release(VpsOwnedMemory *owned_memory)
{
    if (owned_memory == NULL || !owned_memory->owned) {
        return;
    }
    vps_memory_release(&owned_memory->allocator, &owned_memory->memory,
                       owned_memory->size);
    owned_memory->size = 0U;
    owned_memory->owned = 0;
}

VpsMemoryResult vps_size_add(size_t left, size_t right, size_t *result)
{
    if (result == NULL) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    if (right > SIZE_MAX - left) {
        return VPS_MEMORY_OVERFLOW;
    }
    *result = left + right;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_size_multiply(size_t left,
                                  size_t right,
                                  size_t *result)
{
    if (result == NULL) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    if (left != 0U && right > SIZE_MAX / left) {
        return VPS_MEMORY_OVERFLOW;
    }
    *result = left * right;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_size_from_uint64(uint64_t value, size_t *result)
{
    if (result == NULL) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    if (value > (uint64_t)SIZE_MAX) {
        return VPS_MEMORY_RANGE_ERROR;
    }
    *result = (size_t)value;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_size_from_int64(int64_t value, size_t *result)
{
    if (result == NULL) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    if (value < 0) {
        return VPS_MEMORY_RANGE_ERROR;
    }
    return vps_size_from_uint64((uint64_t)value, result);
}

VpsMemoryResult vps_size_to_uint32(size_t value, uint32_t *result)
{
    if (result == NULL) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    if (value > UINT32_MAX) {
        return VPS_MEMORY_RANGE_ERROR;
    }
    *result = (uint32_t)value;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_size_to_int(size_t value, int *result)
{
    if (result == NULL) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    if (value > (size_t)INT_MAX) {
        return VPS_MEMORY_RANGE_ERROR;
    }
    *result = (int)value;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_buffer_init(VpsBuffer *buffer,
                                const VpsAllocator *allocator,
                                size_t limit)
{
    if (buffer == NULL || !vps_allocator_is_valid(allocator)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    buffer->allocator = *allocator;
    buffer->data = NULL;
    buffer->size = 0U;
    buffer->capacity = 0U;
    buffer->limit = limit;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_buffer_reserve(VpsBuffer *buffer, size_t capacity)
{
    void *memory;
    VpsMemoryResult result;

    if (buffer == NULL || !vps_allocator_is_valid(&buffer->allocator)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    if (capacity > buffer->limit) {
        return VPS_MEMORY_LIMIT_EXCEEDED;
    }
    if (capacity <= buffer->capacity) {
        return VPS_MEMORY_OK;
    }

    memory = buffer->data;
    result = vps_memory_reallocate(&buffer->allocator, &memory,
                                   buffer->capacity, capacity);
    if (result != VPS_MEMORY_OK) {
        return result;
    }
    buffer->data = (unsigned char *)memory;
    buffer->capacity = capacity;
    return VPS_MEMORY_OK;
}

VpsMemoryResult vps_buffer_append(VpsBuffer *buffer,
                                 const void *data,
                                 size_t size)
{
    size_t required;
    VpsMemoryResult result;

    if (buffer == NULL || (data == NULL && size != 0U)) {
        return VPS_MEMORY_INVALID_ARGUMENT;
    }
    result = vps_size_add(buffer->size, size, &required);
    if (result != VPS_MEMORY_OK) {
        return result;
    }
    result = vps_buffer_reserve(buffer, required);
    if (result != VPS_MEMORY_OK) {
        return result;
    }
    if (size != 0U) {
        (void)memcpy(buffer->data + buffer->size, data, size);
        buffer->size = required;
    }
    return VPS_MEMORY_OK;
}

void vps_buffer_reset(VpsBuffer *buffer)
{
    void *memory;

    if (buffer == NULL) {
        return;
    }
    memory = buffer->data;
    vps_memory_release(&buffer->allocator, &memory, buffer->capacity);
    buffer->data = NULL;
    buffer->size = 0U;
    buffer->capacity = 0U;
}

const char *vps_memory_result_name(VpsMemoryResult result)
{
    switch (result) {
    case VPS_MEMORY_OK:
        return "ok";
    case VPS_MEMORY_INVALID_ARGUMENT:
        return "invalid_argument";
    case VPS_MEMORY_OVERFLOW:
        return "overflow";
    case VPS_MEMORY_LIMIT_EXCEEDED:
        return "limit_exceeded";
    case VPS_MEMORY_OUT_OF_MEMORY:
        return "out_of_memory";
    case VPS_MEMORY_RANGE_ERROR:
        return "range_error";
    default:
        return "unknown";
    }
}
