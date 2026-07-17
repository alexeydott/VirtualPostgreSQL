#include "vps_platform.h"

#include <stddef.h>

#if defined(_WIN32)
const VpsPlatformOperations *vps_windows_operations(void);
#endif

const VpsPlatformOperations *vps_platform_current_operations(void)
{
#if defined(_WIN32)
    return vps_windows_operations();
#elif defined(__ANDROID__)
    return vps_android_stub_operations();
#else
    return vps_posix_stub_operations();
#endif
}

VpsPlatformStatus vps_platform_validate_operations(
    const VpsPlatformOperations *operations,
    uint64_t required_capabilities)
{
    if (operations == NULL) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    if (operations->structure_size < sizeof(VpsPlatformOperations) ||
        operations->contract_version != VPS_PLATFORM_CONTRACT_VERSION) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    if ((operations->capabilities & required_capabilities) !=
        required_capabilities) {
        return VPS_PLATFORM_UNSUPPORTED;
    }
    return VPS_PLATFORM_OK;
}

#define VPS_PLATFORM_DISPATCH(operations, member, arguments) \
    (((operations) == NULL || (operations)->member == NULL) \
         ? VPS_PLATFORM_UNSUPPORTED \
         : (operations)->member arguments)

VpsPlatformStatus vps_platform_monotonic_now_ms(
    const VpsPlatformOperations *operations,
    uint64_t *milliseconds)
{
    return VPS_PLATFORM_DISPATCH(operations, monotonic_now_ms, (milliseconds));
}

VpsPlatformStatus vps_platform_mutex_init(
    const VpsPlatformOperations *operations,
    VpsPlatformMutex *mutex)
{
    return VPS_PLATFORM_DISPATCH(operations, mutex_init, (mutex));
}

VpsPlatformStatus vps_platform_mutex_destroy(
    const VpsPlatformOperations *operations,
    VpsPlatformMutex *mutex)
{
    return VPS_PLATFORM_DISPATCH(operations, mutex_destroy, (mutex));
}

VpsPlatformStatus vps_platform_mutex_lock(
    const VpsPlatformOperations *operations,
    VpsPlatformMutex *mutex)
{
    return VPS_PLATFORM_DISPATCH(operations, mutex_lock, (mutex));
}

VpsPlatformStatus vps_platform_mutex_unlock(
    const VpsPlatformOperations *operations,
    VpsPlatformMutex *mutex)
{
    return VPS_PLATFORM_DISPATCH(operations, mutex_unlock, (mutex));
}

VpsPlatformStatus vps_platform_condition_init(
    const VpsPlatformOperations *operations,
    VpsPlatformCondition *condition)
{
    return VPS_PLATFORM_DISPATCH(operations, condition_init, (condition));
}

VpsPlatformStatus vps_platform_condition_destroy(
    const VpsPlatformOperations *operations,
    VpsPlatformCondition *condition)
{
    return VPS_PLATFORM_DISPATCH(operations, condition_destroy, (condition));
}

VpsPlatformStatus vps_platform_condition_wait(
    const VpsPlatformOperations *operations,
    VpsPlatformCondition *condition,
    VpsPlatformMutex *mutex,
    uint32_t timeout_ms)
{
    return VPS_PLATFORM_DISPATCH(operations, condition_wait,
                                 (condition, mutex, timeout_ms));
}

VpsPlatformStatus vps_platform_condition_signal(
    const VpsPlatformOperations *operations,
    VpsPlatformCondition *condition)
{
    return VPS_PLATFORM_DISPATCH(operations, condition_signal, (condition));
}

VpsPlatformStatus vps_platform_condition_broadcast(
    const VpsPlatformOperations *operations,
    VpsPlatformCondition *condition)
{
    return VPS_PLATFORM_DISPATCH(operations, condition_broadcast, (condition));
}

VpsPlatformStatus vps_platform_socket_wait(
    const VpsPlatformOperations *operations,
    intptr_t socket_handle,
    VpsWaitInterest interest,
    uint32_t timeout_ms,
    VpsWaitInterest *ready_interest)
{
    return VPS_PLATFORM_DISPATCH(operations, socket_wait,
                                 (socket_handle, interest, timeout_ms,
                                  ready_interest));
}

VpsPlatformStatus vps_platform_secure_zero(
    const VpsPlatformOperations *operations,
    void *buffer,
    size_t buffer_size)
{
    return VPS_PLATFORM_DISPATCH(operations, secure_zero,
                                 (buffer, buffer_size));
}

VpsPlatformStatus vps_platform_environment_get(
    const VpsPlatformOperations *operations,
    const char *name,
    char *buffer,
    size_t buffer_size,
    size_t *required_size)
{
    return VPS_PLATFORM_DISPATCH(operations, environment_get,
                                 (name, buffer, buffer_size, required_size));
}

VpsPlatformStatus vps_platform_file_read(
    const VpsPlatformOperations *operations,
    const char *path,
    uint64_t offset,
    void *buffer,
    size_t buffer_size,
    size_t *bytes_read)
{
    return VPS_PLATFORM_DISPATCH(operations, file_read,
                                 (path, offset, buffer, buffer_size, bytes_read));
}

VpsPlatformStatus vps_platform_entropy_fill(
    const VpsPlatformOperations *operations,
    void *buffer,
    size_t buffer_size)
{
    return VPS_PLATFORM_DISPATCH(operations, entropy_fill,
                                 (buffer, buffer_size));
}
