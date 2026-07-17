#ifndef VPS_MEMORY_H
#define VPS_MEMORY_H

#include <stddef.h>
#include <stdint.h>

typedef enum VpsMemoryResult {
    VPS_MEMORY_OK = 0,
    VPS_MEMORY_INVALID_ARGUMENT = 1,
    VPS_MEMORY_OVERFLOW = 2,
    VPS_MEMORY_LIMIT_EXCEEDED = 3,
    VPS_MEMORY_OUT_OF_MEMORY = 4,
    VPS_MEMORY_RANGE_ERROR = 5
} VpsMemoryResult;

typedef enum VpsAllocatorFamily {
    VPS_ALLOCATOR_FAMILY_SYSTEM = 1,
    VPS_ALLOCATOR_FAMILY_SQLITE = 2,
    VPS_ALLOCATOR_FAMILY_TEST = 3
} VpsAllocatorFamily;

typedef void *(*VpsAllocateFunction)(void *context, size_t size);
typedef void *(*VpsReallocateFunction)(void *context,
                                       void *memory,
                                       size_t old_size,
                                       size_t new_size);
typedef void (*VpsDeallocateFunction)(void *context,
                                      void *memory,
                                      size_t size);

/*
 * The allocator is a copyable, non-owning dispatch table. Its context must
 * outlive every allocation made through the copied table. Memory must be
 * resized and released only through the same allocator family and callbacks.
 * Callbacks may be used concurrently only when their implementation is safe.
 */
typedef struct VpsAllocator {
    VpsAllocatorFamily family;
    void *context;
    VpsAllocateFunction allocate;
    VpsReallocateFunction reallocate;
    VpsDeallocateFunction deallocate;
} VpsAllocator;

/*
 * The buffer owns data through its copied allocator. init creates an empty
 * owner, append/reserve preserve the old allocation on failure, and reset is
 * idempotent. The buffer is not thread-safe and must not be shared mutably.
 */
typedef struct VpsBuffer {
    VpsAllocator allocator;
    unsigned char *data;
    size_t size;
    size_t capacity;
    size_t limit;
} VpsBuffer;

/*
 * A fault allocator is a non-owning wrapper around a backing allocator.
 * fail_at_attempt is one-based across allocate and reallocate callbacks;
 * zero disables injection. The state must outlive all wrapper allocations,
 * is not thread-safe, and may be reset only with no active allocations.
 */
typedef struct VpsFaultAllocator {
    VpsAllocator backing;
    size_t fail_at_attempt;
    size_t attempt_count;
    size_t active_allocations;
    size_t cleanup_errors;
} VpsFaultAllocator;

/*
 * An owned-memory value has exactly one allocator and owner while owned is
 * non-zero. allocate/adopt require an empty owner, detach transfers ownership,
 * and release is idempotent. The value is not thread-safe. Adopted memory must
 * originate from the copied allocator contract.
 */
typedef struct VpsOwnedMemory {
    VpsAllocator allocator;
    void *memory;
    size_t size;
    int owned;
} VpsOwnedMemory;

VpsMemoryResult vps_allocator_init(VpsAllocator *allocator,
                                   VpsAllocatorFamily family,
                                   void *context,
                                   VpsAllocateFunction allocate,
                                   VpsReallocateFunction reallocate,
                                   VpsDeallocateFunction deallocate);
VpsMemoryResult vps_allocator_system(VpsAllocator *allocator);
int vps_allocator_is_valid(const VpsAllocator *allocator);

VpsMemoryResult vps_memory_allocate(const VpsAllocator *allocator,
                                    size_t size,
                                    void **memory);
VpsMemoryResult vps_memory_reallocate(const VpsAllocator *allocator,
                                      void **memory,
                                      size_t old_size,
                                      size_t new_size);
void vps_memory_release(const VpsAllocator *allocator,
                        void **memory,
                        size_t size);

VpsMemoryResult vps_fault_allocator_init(VpsFaultAllocator *fault,
                                         const VpsAllocator *backing,
                                         size_t fail_at_attempt);
VpsMemoryResult vps_fault_allocator_make(VpsFaultAllocator *fault,
                                         VpsAllocator *allocator);
VpsMemoryResult vps_fault_allocator_reset(VpsFaultAllocator *fault,
                                          size_t fail_at_attempt);

VpsMemoryResult vps_owned_memory_init(VpsOwnedMemory *owned_memory,
                                      const VpsAllocator *allocator);
VpsMemoryResult vps_owned_memory_allocate(VpsOwnedMemory *owned_memory,
                                          size_t size);
VpsMemoryResult vps_owned_memory_adopt(VpsOwnedMemory *owned_memory,
                                       void *memory,
                                       size_t size);
VpsMemoryResult vps_owned_memory_detach(VpsOwnedMemory *owned_memory,
                                        void **memory,
                                        size_t *size);
void vps_owned_memory_release(VpsOwnedMemory *owned_memory);

VpsMemoryResult vps_size_add(size_t left, size_t right, size_t *result);
VpsMemoryResult vps_size_multiply(size_t left,
                                  size_t right,
                                  size_t *result);
VpsMemoryResult vps_size_from_uint64(uint64_t value, size_t *result);
VpsMemoryResult vps_size_from_int64(int64_t value, size_t *result);
VpsMemoryResult vps_size_to_uint32(size_t value, uint32_t *result);
VpsMemoryResult vps_size_to_int(size_t value, int *result);

VpsMemoryResult vps_buffer_init(VpsBuffer *buffer,
                                const VpsAllocator *allocator,
                                size_t limit);
VpsMemoryResult vps_buffer_reserve(VpsBuffer *buffer, size_t capacity);
VpsMemoryResult vps_buffer_append(VpsBuffer *buffer,
                                 const void *data,
                                 size_t size);
void vps_buffer_reset(VpsBuffer *buffer);

const char *vps_memory_result_name(VpsMemoryResult result);

#endif
