#include "vps_row_identity.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) return 1; } while (0)

int main(void)
{
    VpsAllocator allocator;
    VpsClientColumnView columns[2];
    VpsBuffer token;
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
              VPS_ROW_IDENTITY_OK && token.size == 17U && token.data[0] == 1U);
    vps_buffer_reset(&token);
    CHECK(vps_row_identity_scan_next(&counter, &rowid) == VPS_ROW_IDENTITY_OK &&
          rowid == 1 &&
          vps_row_identity_scan_next(&counter, &rowid) == VPS_ROW_IDENTITY_OK &&
          rowid == 2);
    (void)printf("vps_row_identity_test status=passed\n");
    return 0;
}
