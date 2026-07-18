#include "vps_planner.h"

#include <stdint.h>
#include <stdio.h>

int main(void)
{
    char input[512];
    uint32_t state = UINT32_C(0x51f15e5d);
    size_t iteration;
    VpsCompiledPlan plan;
    for (iteration = 0U; iteration < 100000U; ++iteration) {
        size_t index;
        size_t length;
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        length = (size_t)(state % sizeof(input));
        for (index = 0U; index < length; ++index) {
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            input[index] = (char)(state & UINT32_C(0xff));
        }
        (void)vps_plan_decode(input, length, UINT64_C(0), &plan);
    }
    (void)printf("planner_fuzz_smoke status=passed iterations=100000\n");
    return 0;
}
