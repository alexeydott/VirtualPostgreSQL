#include "vps_platform.h"

static const VpsPlatformOperations VPS_POSIX_STUB_OPERATIONS = {
    (uint32_t)sizeof(VpsPlatformOperations),
    VPS_PLATFORM_CONTRACT_VERSION,
    UINT64_C(0),
    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL
};

const VpsPlatformOperations *vps_posix_stub_operations(void)
{
    return &VPS_POSIX_STUB_OPERATIONS;
}
