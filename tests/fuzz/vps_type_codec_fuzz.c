#include "vps_type_codec.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    VpsAllocator allocator;
    VpsClientColumnView column;
    VpsDecodedValue decoded;
    VpsCodecId codec;
    if (data == NULL || size == 0U || size > 65536U ||
        vps_allocator_system(&allocator) != VPS_MEMORY_OK)
        return 0;
    codec = (VpsCodecId)(data[0] % 15U);
    (void)memset(&column, 0, sizeof(column));
    column.data = data + 1U;
    column.length = size - 1U;
    column.format = VPS_CLIENT_VALUE_TEXT;
    (void)vps_type_codec_decode(&allocator, codec, &column, &decoded);
    vps_decoded_value_reset(&decoded);
    return 0;
}
