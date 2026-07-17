#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>

#include "vps_platform.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define VPS_WINDOWS_OBJECT_READY UINT32_C(0x56505331)

_Static_assert(sizeof(SRWLOCK) <= VPS_PLATFORM_NATIVE_STORAGE_SIZE,
               "platform mutex storage is too small");
_Static_assert(sizeof(CONDITION_VARIABLE) <= VPS_PLATFORM_NATIVE_STORAGE_SIZE,
               "platform condition storage is too small");

static SRWLOCK *vps_windows_mutex_native(VpsPlatformMutex *mutex)
{
    return (SRWLOCK *)(void *)mutex->native.bytes;
}

static CONDITION_VARIABLE *vps_windows_condition_native(
    VpsPlatformCondition *condition)
{
    return (CONDITION_VARIABLE *)(void *)condition->native.bytes;
}

static wchar_t *vps_windows_utf8_to_wide(const char *value)
{
    int wide_length;
    wchar_t *wide_value;

    if (value == NULL) {
        return NULL;
    }
    wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                                      NULL, 0);
    if (wide_length <= 0 || (size_t)wide_length > SIZE_MAX / sizeof(wchar_t)) {
        return NULL;
    }
    wide_value = (wchar_t *)malloc((size_t)wide_length * sizeof(wchar_t));
    if (wide_value == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                            wide_value, wide_length) != wide_length) {
        free(wide_value);
        return NULL;
    }
    return wide_value;
}

static VpsPlatformStatus vps_windows_monotonic_now_ms(uint64_t *milliseconds)
{
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;

    if (milliseconds == NULL) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter) || counter.QuadPart < 0) {
        return VPS_PLATFORM_SYSTEM_ERROR;
    }
    *milliseconds = ((uint64_t)counter.QuadPart / (uint64_t)frequency.QuadPart) *
                    UINT64_C(1000);
    *milliseconds +=
        (((uint64_t)counter.QuadPart % (uint64_t)frequency.QuadPart) *
         UINT64_C(1000)) /
        (uint64_t)frequency.QuadPart;
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_mutex_init(VpsPlatformMutex *mutex)
{
    if (mutex == NULL) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    if (mutex->state == VPS_WINDOWS_OBJECT_READY) {
        return VPS_PLATFORM_OK;
    }
    (void)memset(mutex, 0, sizeof(*mutex));
    InitializeSRWLock(vps_windows_mutex_native(mutex));
    mutex->state = VPS_WINDOWS_OBJECT_READY;
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_mutex_destroy(VpsPlatformMutex *mutex)
{
    if (mutex == NULL) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    if (mutex->state != 0 && mutex->state != VPS_WINDOWS_OBJECT_READY) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    SecureZeroMemory(mutex, sizeof(*mutex));
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_mutex_lock(VpsPlatformMutex *mutex)
{
    if (mutex == NULL || mutex->state != VPS_WINDOWS_OBJECT_READY) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    AcquireSRWLockExclusive(vps_windows_mutex_native(mutex));
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_mutex_unlock(VpsPlatformMutex *mutex)
{
    if (mutex == NULL || mutex->state != VPS_WINDOWS_OBJECT_READY) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    ReleaseSRWLockExclusive(vps_windows_mutex_native(mutex));
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_condition_init(
    VpsPlatformCondition *condition)
{
    if (condition == NULL) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    if (condition->state == VPS_WINDOWS_OBJECT_READY) {
        return VPS_PLATFORM_OK;
    }
    (void)memset(condition, 0, sizeof(*condition));
    InitializeConditionVariable(vps_windows_condition_native(condition));
    condition->state = VPS_WINDOWS_OBJECT_READY;
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_condition_destroy(
    VpsPlatformCondition *condition)
{
    if (condition == NULL) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    if (condition->state != 0 && condition->state != VPS_WINDOWS_OBJECT_READY) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    SecureZeroMemory(condition, sizeof(*condition));
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_condition_wait(
    VpsPlatformCondition *condition,
    VpsPlatformMutex *mutex,
    uint32_t timeout_ms)
{
    if (condition == NULL || mutex == NULL ||
        condition->state != VPS_WINDOWS_OBJECT_READY ||
        mutex->state != VPS_WINDOWS_OBJECT_READY) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    if (SleepConditionVariableSRW(vps_windows_condition_native(condition),
                                  vps_windows_mutex_native(mutex), timeout_ms,
                                  0)) {
        return VPS_PLATFORM_OK;
    }
    return GetLastError() == ERROR_TIMEOUT ? VPS_PLATFORM_TIMEOUT
                                           : VPS_PLATFORM_SYSTEM_ERROR;
}

static VpsPlatformStatus vps_windows_condition_signal(
    VpsPlatformCondition *condition)
{
    if (condition == NULL || condition->state != VPS_WINDOWS_OBJECT_READY) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    WakeConditionVariable(vps_windows_condition_native(condition));
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_condition_broadcast(
    VpsPlatformCondition *condition)
{
    if (condition == NULL || condition->state != VPS_WINDOWS_OBJECT_READY) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    WakeAllConditionVariable(vps_windows_condition_native(condition));
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_socket_wait(
    intptr_t socket_handle,
    VpsWaitInterest interest,
    uint32_t timeout_ms,
    VpsWaitInterest *ready_interest)
{
    WSAPOLLFD descriptor;
    int result;

    if (ready_interest == NULL || timeout_ms > (uint32_t)INT_MAX ||
        (interest != VPS_WAIT_READ && interest != VPS_WAIT_WRITE &&
         interest != VPS_WAIT_READ_WRITE)) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    descriptor.fd = (SOCKET)socket_handle;
    descriptor.events = 0;
    descriptor.revents = 0;
    if ((interest & VPS_WAIT_READ) != 0) {
        descriptor.events |= POLLRDNORM;
    }
    if ((interest & VPS_WAIT_WRITE) != 0) {
        descriptor.events |= POLLWRNORM;
    }
    result = WSAPoll(&descriptor, 1, (int)timeout_ms);
    if (result == 0) {
        *ready_interest = 0;
        return VPS_PLATFORM_TIMEOUT;
    }
    if (result == SOCKET_ERROR) {
        return VPS_PLATFORM_SYSTEM_ERROR;
    }
    *ready_interest = 0;
    if ((descriptor.revents & (POLLRDNORM | POLLIN)) != 0) {
        *ready_interest = (VpsWaitInterest)(*ready_interest | VPS_WAIT_READ);
    }
    if ((descriptor.revents & (POLLWRNORM | POLLOUT)) != 0) {
        *ready_interest = (VpsWaitInterest)(*ready_interest | VPS_WAIT_WRITE);
    }
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_secure_zero(void *buffer,
                                                  size_t buffer_size)
{
    if (buffer == NULL && buffer_size != 0) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    if (buffer_size != 0) {
        SecureZeroMemory(buffer, buffer_size);
    }
    return VPS_PLATFORM_OK;
}

static VpsPlatformStatus vps_windows_environment_get(
    const char *name,
    char *buffer,
    size_t buffer_size,
    size_t *required_size)
{
    wchar_t *wide_name;
    wchar_t *wide_value;
    DWORD wide_required;
    int utf8_required;
    VpsPlatformStatus status = VPS_PLATFORM_SYSTEM_ERROR;

    if (name == NULL || required_size == NULL || buffer_size > INT_MAX ||
        (buffer == NULL && buffer_size != 0)) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    wide_name = vps_windows_utf8_to_wide(name);
    if (wide_name == NULL) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    wide_required = GetEnvironmentVariableW(wide_name, NULL, 0);
    if (wide_required == 0) {
        free(wide_name);
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? VPS_PLATFORM_NOT_FOUND
                                                        : VPS_PLATFORM_SYSTEM_ERROR;
    }
    wide_value = (wchar_t *)malloc((size_t)wide_required * sizeof(wchar_t));
    if (wide_value == NULL) {
        free(wide_name);
        return VPS_PLATFORM_SYSTEM_ERROR;
    }
    if (GetEnvironmentVariableW(wide_name, wide_value, wide_required) == 0) {
        goto cleanup;
    }
    utf8_required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                        wide_value, -1, NULL, 0, NULL, NULL);
    if (utf8_required <= 0) {
        goto cleanup;
    }
    *required_size = (size_t)utf8_required;
    if (buffer_size < *required_size) {
        status = VPS_PLATFORM_BUFFER_TOO_SMALL;
        goto cleanup;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_value, -1,
                            buffer, (int)buffer_size, NULL, NULL) !=
        utf8_required) {
        goto cleanup;
    }
    status = VPS_PLATFORM_OK;

cleanup:
    SecureZeroMemory(wide_value, (size_t)wide_required * sizeof(wchar_t));
    free(wide_value);
    free(wide_name);
    return status;
}

static VpsPlatformStatus vps_windows_file_read(
    const char *path,
    uint64_t offset,
    void *buffer,
    size_t buffer_size,
    size_t *bytes_read)
{
    wchar_t *wide_path;
    HANDLE file_handle;
    LARGE_INTEGER file_offset;
    DWORD read_size;
    VpsPlatformStatus status = VPS_PLATFORM_SYSTEM_ERROR;

    if (path == NULL || bytes_read == NULL ||
        (buffer == NULL && buffer_size != 0) || buffer_size > UINT32_MAX ||
        offset > INT64_MAX) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    *bytes_read = 0;
    wide_path = vps_windows_utf8_to_wide(path);
    if (wide_path == NULL) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    file_handle = CreateFileW(wide_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide_path);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? VPS_PLATFORM_NOT_FOUND
                                                       : VPS_PLATFORM_SYSTEM_ERROR;
    }
    file_offset.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file_handle, file_offset, NULL, FILE_BEGIN)) {
        goto cleanup;
    }
    if (!ReadFile(file_handle, buffer, (DWORD)buffer_size, &read_size, NULL)) {
        goto cleanup;
    }
    *bytes_read = (size_t)read_size;
    status = VPS_PLATFORM_OK;

cleanup:
    (void)CloseHandle(file_handle);
    return status;
}

static VpsPlatformStatus vps_windows_entropy_fill(void *buffer,
                                                   size_t buffer_size)
{
    if ((buffer == NULL && buffer_size != 0) || buffer_size > UINT32_MAX) {
        return VPS_PLATFORM_INVALID_ARGUMENT;
    }
    if (buffer_size == 0) {
        return VPS_PLATFORM_OK;
    }
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)buffer,
                                          (ULONG)buffer_size,
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG))
               ? VPS_PLATFORM_OK
               : VPS_PLATFORM_SYSTEM_ERROR;
}

static const VpsPlatformOperations VPS_WINDOWS_OPERATIONS = {
    (uint32_t)sizeof(VpsPlatformOperations),
    VPS_PLATFORM_CONTRACT_VERSION,
    VPS_PLATFORM_CAP_MONOTONIC_CLOCK | VPS_PLATFORM_CAP_MUTEX |
        VPS_PLATFORM_CAP_CONDITION | VPS_PLATFORM_CAP_SOCKET_WAIT |
        VPS_PLATFORM_CAP_SECURE_ZERO | VPS_PLATFORM_CAP_ENVIRONMENT |
        VPS_PLATFORM_CAP_FILE_READ | VPS_PLATFORM_CAP_ENTROPY,
    vps_windows_monotonic_now_ms,
    vps_windows_mutex_init,
    vps_windows_mutex_destroy,
    vps_windows_mutex_lock,
    vps_windows_mutex_unlock,
    vps_windows_condition_init,
    vps_windows_condition_destroy,
    vps_windows_condition_wait,
    vps_windows_condition_signal,
    vps_windows_condition_broadcast,
    vps_windows_socket_wait,
    vps_windows_secure_zero,
    vps_windows_environment_get,
    vps_windows_file_read,
    vps_windows_entropy_fill
};

const VpsPlatformOperations *vps_windows_operations(void)
{
    return &VPS_WINDOWS_OPERATIONS;
}
