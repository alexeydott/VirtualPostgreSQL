#include "vps_spatial.h"

#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    VpsSpatialLimits limits = {65536U, 10000U, 32U};
    VpsSpatialValidation validation;
    if (data == NULL || size == 0U || size > limits.max_bytes) return 0;
    if ((data[0] & 1U) == 0U)
        (void)vps_spatial_validate_text(
            (const char *)(data + 1U), size - 1U,
            (data[0] & 2U) != 0U ? VPS_SPATIAL_FORMAT_EWKT
                                 : VPS_SPATIAL_FORMAT_WKT,
            VPS_SPATIAL_TYPE_ANY, 0U, &limits, &validation);
    else
        (void)vps_spatial_validate_binary(
            data + 1U, size - 1U,
            (data[0] & 2U) != 0U ? VPS_SPATIAL_FORMAT_EWKB
                                 : VPS_SPATIAL_FORMAT_WKB,
            VPS_SPATIAL_TYPE_ANY, 0U, &limits, &validation);
    return 0;
}
