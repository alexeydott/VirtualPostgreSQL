#include "vps_type_codec.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    (void)fprintf(stderr, "type_codec check failed line=%d\n", __LINE__); \
    return 1; } } while (0)

static VpsClientColumnView column(const char *value, uint32_t oid)
{
    VpsClientColumnView result;
    (void)memset(&result, 0, sizeof(result));
    result.data = value;
    result.length = value == NULL ? 0U : strlen(value);
    result.type_oid = oid;
    result.format = VPS_CLIENT_VALUE_TEXT;
    result.is_null = value == NULL;
    return result;
}

int main(void)
{
    VpsAllocator allocator;
    VpsDecodedValue value;
    VpsClientColumnView input;
    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    input = column(NULL, 25U);
    CHECK(vps_type_codec_decode(&allocator, VPS_CODEC_TEXT, &input, &value) ==
          VPS_TYPE_CODEC_OK && value.kind == VPS_DECODED_NULL);
    vps_decoded_value_reset(&value);
    input = column("t", 16U);
    CHECK(vps_type_codec_decode(&allocator, VPS_CODEC_BOOLEAN, &input, &value) ==
          VPS_TYPE_CODEC_OK && value.integer == 1);
    vps_decoded_value_reset(&value);
    input = column("-9223372036854775808", 20U);
    CHECK(vps_type_codec_decode(&allocator, VPS_CODEC_INTEGER, &input, &value) ==
          VPS_TYPE_CODEC_OK && value.integer == INT64_MIN);
    vps_decoded_value_reset(&value);
    input = column("NaN", 701U);
    CHECK(vps_type_codec_decode(&allocator, VPS_CODEC_FLOAT, &input, &value) ==
          VPS_TYPE_CODEC_OK && value.kind == VPS_DECODED_TEXT);
    vps_decoded_value_reset(&value);
    input = column("\\x0041ff", 17U);
    CHECK(vps_type_codec_decode(&allocator, VPS_CODEC_BYTEA, &input, &value) ==
          VPS_TYPE_CODEC_OK && value.kind == VPS_DECODED_BLOB &&
          value.length == 3U && ((const unsigned char *)value.bytes)[0] == 0U);
    vps_decoded_value_reset(&value);
    input = column("550e8400-e29b-41d4-a716-446655440000", 2950U);
    CHECK(vps_type_codec_decode(&allocator, VPS_CODEC_UUID_TEXT, &input, &value) ==
          VPS_TYPE_CODEC_OK);
    vps_decoded_value_reset(&value);
    input = column("550E8400-e29b-41d4-a716-446655440000", 2950U);
    CHECK(vps_type_codec_decode(&allocator, VPS_CODEC_UUID_TEXT, &input, &value) ==
          VPS_TYPE_CODEC_MALFORMED);
    (void)printf("vps_type_codec_test status=passed\n");
    return 0;
}
