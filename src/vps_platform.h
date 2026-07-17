#ifndef VPS_PLATFORM_H
#define VPS_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#define VPS_PLATFORM_NATIVE_STORAGE_SIZE 64

#define VPS_PLATFORM_CAP_MONOTONIC_CLOCK (UINT64_C(1) << 0)
#define VPS_PLATFORM_CAP_MUTEX           (UINT64_C(1) << 1)
#define VPS_PLATFORM_CAP_CONDITION       (UINT64_C(1) << 2)
#define VPS_PLATFORM_CAP_SOCKET_WAIT     (UINT64_C(1) << 3)
#define VPS_PLATFORM_CAP_SECURE_ZERO     (UINT64_C(1) << 4)
#define VPS_PLATFORM_CAP_ENVIRONMENT     (UINT64_C(1) << 5)
#define VPS_PLATFORM_CAP_FILE_READ       (UINT64_C(1) << 6)
#define VPS_PLATFORM_CAP_ENTROPY         (UINT64_C(1) << 7)
#define VPS_PLATFORM_CAP_ALL             (UINT64_C(0xff))

typedef enum VpsPlatformStatus {
    VPS_PLATFORM_OK = 0,
    VPS_PLATFORM_TIMEOUT = 1,
    VPS_PLATFORM_UNSUPPORTED = 2,
    VPS_PLATFORM_INVALID_ARGUMENT = 3,
    VPS_PLATFORM_BUFFER_TOO_SMALL = 4,
    VPS_PLATFORM_NOT_FOUND = 5,
    VPS_PLATFORM_SYSTEM_ERROR = 6
} VpsPlatformStatus;

typedef enum VpsWaitInterest {
    VPS_WAIT_READ = 1,
    VPS_WAIT_WRITE = 2,
    VPS_WAIT_READ_WRITE = 3
} VpsWaitInterest;

typedef union VpsPlatformNativeStorage {
    long double long_double_alignment;
    void *pointer_alignment;
    uint64_t integer_alignment;
    unsigned char bytes[VPS_PLATFORM_NATIVE_STORAGE_SIZE];
} VpsPlatformNativeStorage;

typedef struct VpsPlatformMutex {
    uint32_t state;
    VpsPlatformNativeStorage native;
} VpsPlatformMutex;

typedef struct VpsPlatformCondition {
    uint32_t state;
    VpsPlatformNativeStorage native;
} VpsPlatformCondition;

typedef struct VpsPlatformOperations {
    uint32_t structure_size;
    uint32_t contract_version;
    uint64_t capabilities;
    VpsPlatformStatus (*monotonic_now_ms)(uint64_t *milliseconds);
    VpsPlatformStatus (*mutex_init)(VpsPlatformMutex *mutex);
    VpsPlatformStatus (*mutex_destroy)(VpsPlatformMutex *mutex);
    VpsPlatformStatus (*mutex_lock)(VpsPlatformMutex *mutex);
    VpsPlatformStatus (*mutex_unlock)(VpsPlatformMutex *mutex);
    VpsPlatformStatus (*condition_init)(VpsPlatformCondition *condition);
    VpsPlatformStatus (*condition_destroy)(VpsPlatformCondition *condition);
    VpsPlatformStatus (*condition_wait)(VpsPlatformCondition *condition,
                                        VpsPlatformMutex *mutex,
                                        uint32_t timeout_ms);
    VpsPlatformStatus (*condition_signal)(VpsPlatformCondition *condition);
    VpsPlatformStatus (*condition_broadcast)(VpsPlatformCondition *condition);
    VpsPlatformStatus (*socket_wait)(intptr_t socket_handle,
                                     VpsWaitInterest interest,
                                     uint32_t timeout_ms,
                                     VpsWaitInterest *ready_interest);
    VpsPlatformStatus (*secure_zero)(void *buffer, size_t buffer_size);
    VpsPlatformStatus (*environment_get)(const char *name,
                                         char *buffer,
                                         size_t buffer_size,
                                         size_t *required_size);
    VpsPlatformStatus (*file_read)(const char *path,
                                   uint64_t offset,
                                   void *buffer,
                                   size_t buffer_size,
                                   size_t *bytes_read);
    VpsPlatformStatus (*entropy_fill)(void *buffer, size_t buffer_size);
} VpsPlatformOperations;

#define VPS_PLATFORM_CONTRACT_VERSION UINT32_C(1)

const VpsPlatformOperations *vps_platform_current_operations(void);
const VpsPlatformOperations *vps_posix_stub_operations(void);
const VpsPlatformOperations *vps_android_stub_operations(void);

VpsPlatformStatus vps_platform_validate_operations(
    const VpsPlatformOperations *operations,
    uint64_t required_capabilities);
VpsPlatformStatus vps_platform_monotonic_now_ms(
    const VpsPlatformOperations *operations,
    uint64_t *milliseconds);
VpsPlatformStatus vps_platform_mutex_init(
    const VpsPlatformOperations *operations,
    VpsPlatformMutex *mutex);
VpsPlatformStatus vps_platform_mutex_destroy(
    const VpsPlatformOperations *operations,
    VpsPlatformMutex *mutex);
VpsPlatformStatus vps_platform_mutex_lock(
    const VpsPlatformOperations *operations,
    VpsPlatformMutex *mutex);
VpsPlatformStatus vps_platform_mutex_unlock(
    const VpsPlatformOperations *operations,
    VpsPlatformMutex *mutex);
VpsPlatformStatus vps_platform_condition_init(
    const VpsPlatformOperations *operations,
    VpsPlatformCondition *condition);
VpsPlatformStatus vps_platform_condition_destroy(
    const VpsPlatformOperations *operations,
    VpsPlatformCondition *condition);
VpsPlatformStatus vps_platform_condition_wait(
    const VpsPlatformOperations *operations,
    VpsPlatformCondition *condition,
    VpsPlatformMutex *mutex,
    uint32_t timeout_ms);
VpsPlatformStatus vps_platform_condition_signal(
    const VpsPlatformOperations *operations,
    VpsPlatformCondition *condition);
VpsPlatformStatus vps_platform_condition_broadcast(
    const VpsPlatformOperations *operations,
    VpsPlatformCondition *condition);
VpsPlatformStatus vps_platform_socket_wait(
    const VpsPlatformOperations *operations,
    intptr_t socket_handle,
    VpsWaitInterest interest,
    uint32_t timeout_ms,
    VpsWaitInterest *ready_interest);
VpsPlatformStatus vps_platform_secure_zero(
    const VpsPlatformOperations *operations,
    void *buffer,
    size_t buffer_size);
VpsPlatformStatus vps_platform_environment_get(
    const VpsPlatformOperations *operations,
    const char *name,
    char *buffer,
    size_t buffer_size,
    size_t *required_size);
VpsPlatformStatus vps_platform_file_read(
    const VpsPlatformOperations *operations,
    const char *path,
    uint64_t offset,
    void *buffer,
    size_t buffer_size,
    size_t *bytes_read);
VpsPlatformStatus vps_platform_entropy_fill(
    const VpsPlatformOperations *operations,
    void *buffer,
    size_t buffer_size);

#endif
