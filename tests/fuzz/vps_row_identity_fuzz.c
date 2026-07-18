#include "vps_row_identity.h"

#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    VpsRowIdentityView view;
    if (data != NULL && size <= VPS_ROW_IDENTITY_MAX_BYTES)
        (void)vps_row_identity_decode(data, size, &view);
    return 0;
}
