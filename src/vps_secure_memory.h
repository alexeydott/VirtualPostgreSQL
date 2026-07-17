#ifndef VPS_SECURE_MEMORY_H
#define VPS_SECURE_MEMORY_H

#include "vps_logging.h"
#include "vps_memory.h"
#include "vps_platform.h"

#include <stddef.h>

typedef enum VpsSecureMemoryResult {
    VPS_SECURE_MEMORY_OK = 0,
    VPS_SECURE_MEMORY_INVALID_ARGUMENT = 1,
    VPS_SECURE_MEMORY_OUT_OF_MEMORY = 2,
    VPS_SECURE_MEMORY_ZERO_FAILED = 3
} VpsSecureMemoryResult;

/*
 * Sensitive memory owns one allocation through storage and borrows both the
 * platform operations and optional logger. Cleanup securely zeroes the whole
 * allocation before invoking its matching deallocator. If zeroing fails,
 * ownership is retained so the caller can retry without releasing secret
 * bytes. Successful and empty cleanup are idempotent. This value is not
 * thread-safe.
 */
typedef struct VpsSensitiveMemory {
    VpsOwnedMemory storage;
    const VpsPlatformOperations *operations;
    VpsLogger *logger;
    int initialized;
} VpsSensitiveMemory;

VpsSecureMemoryResult vps_sensitive_memory_init(
    VpsSensitiveMemory *sensitive_memory,
    const VpsAllocator *allocator,
    const VpsPlatformOperations *operations,
    VpsLogger *logger);
VpsSecureMemoryResult vps_sensitive_memory_allocate(
    VpsSensitiveMemory *sensitive_memory,
    size_t size);
VpsSecureMemoryResult vps_sensitive_memory_adopt(
    VpsSensitiveMemory *sensitive_memory,
    void *memory,
    size_t size);
VpsSecureMemoryResult vps_sensitive_memory_release(
    VpsSensitiveMemory *sensitive_memory);

void *vps_sensitive_memory_data(VpsSensitiveMemory *sensitive_memory);
size_t vps_sensitive_memory_size(const VpsSensitiveMemory *sensitive_memory);
const char *vps_secure_memory_result_name(VpsSecureMemoryResult result);

#endif
