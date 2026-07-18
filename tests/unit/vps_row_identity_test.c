#include "vps_row_identity.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) return 1; } while (0)

int main(void)
{
    VpsAllocator allocator;
    VpsClientColumnView columns[2];
    VpsBuffer token;
    VpsRowIdentityField fields[3];
    VpsRowIdentityField optimistic;
    VpsRowIdentitySpec spec;
    VpsRowIdentityView view;
    unsigned char fuzz[VPS_ROW_IDENTITY_MAX_FIELDS * 16U];
    size_t iteration;
    uint64_t counter = 0U;
    int64_t rowid = 0;
    (void)memset(columns, 0, sizeof(columns));
    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    columns[0].data = "-7";
    columns[0].length = 2U;
    CHECK(vps_row_identity_stable_integer(&columns[0], &rowid) ==
              VPS_ROW_IDENTITY_OK && rowid == -7);
    columns[0].data = "alpha";
    columns[0].length = 5U;
    columns[1].is_null = 1;
    CHECK(vps_row_identity_token(&allocator, columns, 2U, &token) ==
              VPS_ROW_IDENTITY_OK && token.size > 17U && token.data[0] == 'V');
    CHECK(vps_row_identity_decode(token.data, token.size, &view) ==
              VPS_ROW_IDENTITY_OK && view.key_field_count == 2U &&
          view.key_fields[0].length == 5U &&
          view.key_fields[1].kind == VPS_ROW_IDENTITY_FIELD_NULL);
    vps_buffer_reset(&token);
    (void)memset(fields, 0, sizeof(fields));
    fields[0].kind = VPS_ROW_IDENTITY_FIELD_TEXT;
    fields[0].type_oid = 2950U;
    fields[0].bytes = "550e8400-e29b-41d4-a716-446655440000";
    fields[0].length = 36U;
    fields[1].kind = VPS_ROW_IDENTITY_FIELD_NULL;
    fields[1].type_oid = 25U;
    fields[2].kind = VPS_ROW_IDENTITY_FIELD_INTEGER;
    fields[2].type_oid = 20U;
    fields[2].integer = INT64_MIN;
    optimistic.kind = VPS_ROW_IDENTITY_FIELD_TEXT;
    optimistic.type_oid = 25U;
    optimistic.bytes = "v7";
    optimistic.length = 2U;
    spec.relation_oid = 42U;
    spec.key_fields = fields;
    spec.key_field_count = 3U;
    spec.optimistic_field = &optimistic;
    CHECK(vps_row_identity_encode(&allocator, &spec, &token) ==
              VPS_ROW_IDENTITY_OK);
    CHECK(vps_row_identity_decode(token.data, token.size, &view) ==
              VPS_ROW_IDENTITY_OK && view.relation_oid == 42U &&
          view.key_field_count == 3U && view.has_optimistic_field &&
          view.key_fields[2].integer == INT64_MIN &&
          view.optimistic_field.length == 2U);
    CHECK(token.size <= sizeof(fuzz));
    for (iteration = 0U; iteration < 1000000U; ++iteration) {
        VpsRowIdentityResult fuzz_result;
        size_t byte = (iteration * 17U + 3U) % token.size;
        (void)memcpy(fuzz, token.data, token.size);
        fuzz[byte] ^= (unsigned char)(1U + (iteration % 255U));
        fuzz_result = vps_row_identity_decode(fuzz, token.size, &view);
        CHECK(fuzz_result >= VPS_ROW_IDENTITY_OK &&
              fuzz_result <= VPS_ROW_IDENTITY_OUT_OF_MEMORY);
    }
    CHECK(vps_row_identity_decode(token.data, token.size - 1U, &view) ==
              VPS_ROW_IDENTITY_MALFORMED);
    token.data[token.size - 1U] ^= 1U;
    CHECK(vps_row_identity_decode(token.data, token.size, &view) ==
              VPS_ROW_IDENTITY_OK);
    token.data[4] = 0xffU;
    CHECK(vps_row_identity_decode(token.data, token.size, &view) ==
              VPS_ROW_IDENTITY_MALFORMED);
    vps_buffer_reset(&token);
    CHECK(vps_row_identity_scan_next(&counter, &rowid) == VPS_ROW_IDENTITY_OK &&
          rowid == 1);
    CHECK(vps_row_identity_scan_next(&counter, &rowid) == VPS_ROW_IDENTITY_OK &&
          rowid == 2);
    (void)printf("vps_row_identity_test status=passed\n");
    return 0;
}
