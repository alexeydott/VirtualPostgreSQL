#include "vps_planner.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    VpsCompiledPlan plan;
    if (data != NULL)
        (void)vps_plan_decode((const char *)data, size, UINT64_C(0), &plan);
    return 0;
}
