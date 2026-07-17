#include "vps_platform.h"

#include <stdio.h>
#include <string.h>

static int vps_expect(int condition, const char *operation)
{
    if (!condition) {
        (void)fprintf(stderr,
                      "[platform] level=error operation=%s status=failed\n",
                      operation);
        return 0;
    }
    return 1;
}

int main(void)
{
    const VpsPlatformOperations *operations = vps_platform_current_operations();
    const VpsPlatformOperations *posix_stub = vps_posix_stub_operations();
    const VpsPlatformOperations *android_stub = vps_android_stub_operations();
    VpsPlatformMutex mutex = {0};
    VpsPlatformCondition condition = {0};
    unsigned char sensitive[32];
    unsigned char entropy[32];
    unsigned char file_buffer[4];
    uint64_t started_ms = 0;
    uint64_t finished_ms = 0;
    int passed = 1;
    size_t index;
    size_t required_size = 0;
    size_t bytes_read = 0;
    VpsWaitInterest ready_interest = 0;
    FILE *fixture;

    (void)memset(sensitive, 0xa5, sizeof(sensitive));
    (void)memset(entropy, 0, sizeof(entropy));
    passed &= vps_expect(
        vps_platform_validate_operations(posix_stub,
                                         VPS_PLATFORM_CAP_MONOTONIC_CLOCK) ==
            VPS_PLATFORM_UNSUPPORTED,
        "posix_stub_is_explicit");
    passed &= vps_expect(
        vps_platform_validate_operations(android_stub,
                                         VPS_PLATFORM_CAP_MONOTONIC_CLOCK) ==
            VPS_PLATFORM_UNSUPPORTED,
        "android_stub_is_explicit");

#if defined(_WIN32)
    passed &= vps_expect(
        vps_platform_validate_operations(operations, VPS_PLATFORM_CAP_ALL) ==
            VPS_PLATFORM_OK,
        "windows_capabilities");
    passed &= vps_expect(
        vps_platform_monotonic_now_ms(operations, &started_ms) ==
            VPS_PLATFORM_OK,
        "monotonic_start");
    passed &= vps_expect(
        vps_platform_mutex_init(operations, &mutex) == VPS_PLATFORM_OK,
        "mutex_init");
    passed &= vps_expect(
        vps_platform_mutex_lock(operations, &mutex) == VPS_PLATFORM_OK,
        "mutex_lock");
    passed &= vps_expect(
        vps_platform_condition_init(operations, &condition) == VPS_PLATFORM_OK,
        "condition_init");
    passed &= vps_expect(
        vps_platform_condition_wait(operations, &condition, &mutex, 1) ==
            VPS_PLATFORM_TIMEOUT,
        "condition_timeout");
    passed &= vps_expect(
        vps_platform_condition_signal(operations, &condition) ==
            VPS_PLATFORM_OK,
        "condition_signal");
    passed &= vps_expect(
        vps_platform_condition_broadcast(operations, &condition) ==
            VPS_PLATFORM_OK,
        "condition_broadcast");
    passed &= vps_expect(
        vps_platform_mutex_unlock(operations, &mutex) == VPS_PLATFORM_OK,
        "mutex_unlock");
    passed &= vps_expect(
        vps_platform_condition_destroy(operations, &condition) ==
            VPS_PLATFORM_OK,
        "condition_destroy");
    passed &= vps_expect(
        vps_platform_condition_destroy(operations, &condition) ==
            VPS_PLATFORM_OK,
        "condition_destroy_repeat");
    passed &= vps_expect(
        vps_platform_mutex_destroy(operations, &mutex) == VPS_PLATFORM_OK,
        "mutex_destroy");
    passed &= vps_expect(
        vps_platform_mutex_destroy(operations, &mutex) == VPS_PLATFORM_OK,
        "mutex_destroy_repeat");
    passed &= vps_expect(
        vps_platform_entropy_fill(operations, entropy, sizeof(entropy)) ==
            VPS_PLATFORM_OK,
        "entropy_fill");
    passed &= vps_expect(
        vps_platform_environment_get(operations, "PATH", NULL, 0,
                                     &required_size) ==
                VPS_PLATFORM_BUFFER_TOO_SMALL &&
            required_size > 1,
        "environment_bounded_query");
#if defined(_WIN32)
    passed &= vps_expect(
        fopen_s(&fixture, "vps_platform_fixture.bin", "wb") == 0,
        "file_fixture_open_call");
#else
    fixture = fopen("vps_platform_fixture.bin", "wb");
#endif
    passed &= vps_expect(fixture != NULL, "file_fixture_open");
    if (fixture != NULL) {
        passed &= vps_expect(fwrite("VPS1", 1, 4, fixture) == 4,
                             "file_fixture_write");
        passed &= vps_expect(fclose(fixture) == 0, "file_fixture_close");
        passed &= vps_expect(
            vps_platform_file_read(operations, "vps_platform_fixture.bin", 0,
                                   file_buffer, sizeof(file_buffer),
                                   &bytes_read) == VPS_PLATFORM_OK &&
                bytes_read == sizeof(file_buffer) &&
                memcmp(file_buffer, "VPS1", sizeof(file_buffer)) == 0,
            "file_read");
        passed &= vps_expect(remove("vps_platform_fixture.bin") == 0,
                             "file_fixture_remove");
    }
    passed &= vps_expect(
        vps_platform_socket_wait(operations, -1, (VpsWaitInterest)0, 0,
                                 &ready_interest) ==
            VPS_PLATFORM_INVALID_ARGUMENT,
        "socket_wait_argument_validation");
    passed &= vps_expect(
        vps_platform_secure_zero(operations, sensitive, sizeof(sensitive)) ==
            VPS_PLATFORM_OK,
        "secure_zero");
    for (index = 0; index < sizeof(sensitive); ++index) {
        passed &= vps_expect(sensitive[index] == 0, "secure_zero_content");
    }
    passed &= vps_expect(
        vps_platform_monotonic_now_ms(operations, &finished_ms) ==
            VPS_PLATFORM_OK &&
            finished_ms >= started_ms,
        "monotonic_finish");
#else
    passed &= vps_expect(operations == posix_stub || operations == android_stub,
                         "portable_stub_selected");
    passed &= vps_expect(
        vps_platform_monotonic_now_ms(operations, &started_ms) ==
            VPS_PLATFORM_UNSUPPORTED,
        "portable_stub_dispatch");
#endif

    (void)printf(
        "[platform] level=info operation=contract duration_ms=%llu "
        "status=%s\n",
        (unsigned long long)(finished_ms - started_ms),
        passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
