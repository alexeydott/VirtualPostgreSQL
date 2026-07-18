#include "vps_query_source.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    VpsQuerySourceAnalysis analysis;
    if (data == NULL || size == 0U || size > VPS_QUERY_SOURCE_MAX_BYTES) {
        return 0;
    }
    (void)vps_query_source_scan((const char *)data, size, NULL, &analysis);
    return 0;
}
