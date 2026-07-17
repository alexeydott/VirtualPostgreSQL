#include "vps_platform.h"

static const VpsPlatformOperations VPS_ANDROID_STUB_OPERATIONS = {
    (uint32_t)sizeof(VpsPlatformOperations),
    VPS_PLATFORM_CONTRACT_VERSION,
    UINT64_C(0),
    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL
};

const VpsPlatformOperations *vps_android_stub_operations(void)
{
    return &VPS_ANDROID_STUB_OPERATIONS;
}
