#include "vps_embedded_sqlite.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value)                                                           \
    do {                                                                       \
        if (!(value)) {                                                        \
            (void)fprintf(stderr, "[embedded-sqlite] check failed: %s:%d\n",   \
                          __FILE__, __LINE__);                                  \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(void)
{
    VpsAllocator allocator;
    VpsEmbeddedSqliteOpenOptions options;
    VpsEmbeddedSqlite *database = NULL;
    VpsFaultAllocator fault;
    VpsAllocator fault_allocator;
    VpsEmbeddedValueKind kinds[] = {
        VPS_EMBEDDED_VALUE_TEXT, VPS_EMBEDDED_VALUE_INTEGER,
        VPS_EMBEDDED_VALUE_REAL, VPS_EMBEDDED_VALUE_TEXT,
        VPS_EMBEDDED_VALUE_BLOB};
    VpsEmbeddedIndexDefinition index_definition;
    VpsEmbeddedSchema schema;
    static const unsigned char blob_value[] = {0x00U, 0x7fU, 0xffU};
    VpsEmbeddedValue rows[2][5];
    uint16_t projection[] = {0U, 1U, 2U, 3U, 4U};
    VpsEmbeddedConstraint constraint;
    VpsEmbeddedScanRequest request;
    VpsEmbeddedSqliteScan *scan = NULL;
    VpsEmbeddedValue value;
    int has_row = 0;
    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK);
    options.allocator = allocator;
    options.logger = NULL;
    options.mode = VPS_EMBEDDED_SQLITE_MEMORY;
    options.temp_path = NULL;
    options.temp_path_length = 0U;
    CHECK(vps_embedded_sqlite_open(&options, &database) ==
          VPS_EMBEDDED_SQLITE_OK);
    CHECK(database != NULL);
    CHECK(vps_embedded_sqlite_version_number() >= 3044000);
    CHECK(vps_embedded_sqlite_version()[0] != '\0');
    CHECK(vps_embedded_sqlite_compile_options_fingerprint() != 0U);
    (void)memset(&index_definition, 0, sizeof(index_definition));
    index_definition.columns[0] = 1U;
    index_definition.columns[1] = 3U;
    index_definition.column_count = 2U;
    index_definition.name_hash = UINT64_C(1234);
    schema.column_kinds = kinds;
    schema.column_count = 5U;
    schema.indexes = &index_definition;
    schema.index_count = 1U;
    schema.source_fingerprint = UINT64_C(10);
    schema.layout_fingerprint = UINT64_C(20);
    CHECK(vps_embedded_sqlite_create_schema(database, &schema) ==
          VPS_EMBEDDED_SQLITE_OK);
    CHECK(vps_embedded_sqlite_source_fingerprint(database) == UINT64_C(10));
    CHECK(vps_embedded_sqlite_layout_fingerprint(database) == UINT64_C(20));
    (void)memset(rows, 0, sizeof(rows));
    rows[0][0].kind = VPS_EMBEDDED_VALUE_NULL;
    rows[0][1].kind = VPS_EMBEDDED_VALUE_INTEGER;
    rows[0][1].integer = 7;
    rows[0][2].kind = VPS_EMBEDDED_VALUE_REAL;
    rows[0][2].real = 1.25;
    rows[0][3].kind = VPS_EMBEDDED_VALUE_TEXT;
    rows[0][3].bytes = "12345678901234567890.125";
    rows[0][3].length = 24U;
    rows[0][4].kind = VPS_EMBEDDED_VALUE_BLOB;
    rows[0][4].bytes = blob_value;
    rows[0][4].length = sizeof(blob_value);
    rows[1][0].kind = VPS_EMBEDDED_VALUE_TEXT;
    rows[1][0].bytes = "empty";
    rows[1][0].length = 5U;
    rows[1][1].kind = VPS_EMBEDDED_VALUE_INTEGER;
    rows[1][1].integer = 8;
    rows[1][2].kind = VPS_EMBEDDED_VALUE_REAL;
    rows[1][2].real = -2.5;
    rows[1][3].kind = VPS_EMBEDDED_VALUE_TEXT;
    rows[1][3].bytes = "0";
    rows[1][3].length = 1U;
    rows[1][4].kind = VPS_EMBEDDED_VALUE_BLOB;
    rows[1][4].bytes = "";
    rows[1][4].length = 0U;
    CHECK(vps_embedded_sqlite_append_row(database, rows[0], 5U) ==
          VPS_EMBEDDED_SQLITE_OK);
    CHECK(vps_embedded_sqlite_append_row(database, rows[1], 5U) ==
          VPS_EMBEDDED_SQLITE_OK);
    CHECK(vps_embedded_sqlite_seal(database) == VPS_EMBEDDED_SQLITE_OK);
    CHECK(vps_embedded_sqlite_row_count(database) == 2U);
    CHECK(vps_embedded_sqlite_byte_count(database) == 33U);
    (void)memset(&constraint, 0, sizeof(constraint));
    constraint.column = 1U;
    constraint.operation = VPS_EMBEDDED_OP_EQ;
    constraint.value.kind = VPS_EMBEDDED_VALUE_INTEGER;
    constraint.value.integer = 7;
    (void)memset(&request, 0, sizeof(request));
    request.projection = projection;
    request.projection_count = 5U;
    request.constraints = &constraint;
    request.constraint_count = 1U;
    request.selected_index = 0U;
    request.use_index = 1;
    CHECK(vps_embedded_sqlite_scan_open(database, &request, &scan) ==
          VPS_EMBEDDED_SQLITE_OK);
    CHECK(vps_embedded_sqlite_scan_uses_index(scan));
    CHECK(vps_embedded_sqlite_scan_step(scan, &has_row) ==
              VPS_EMBEDDED_SQLITE_OK && has_row);
    CHECK(vps_embedded_sqlite_scan_column(scan, 0U, &value) ==
              VPS_EMBEDDED_SQLITE_OK &&
          value.kind == VPS_EMBEDDED_VALUE_NULL);
    CHECK(vps_embedded_sqlite_scan_column(scan, 3U, &value) ==
              VPS_EMBEDDED_SQLITE_OK &&
          value.kind == VPS_EMBEDDED_VALUE_TEXT && value.length == 24U &&
          memcmp(value.bytes, "12345678901234567890.125", 24U) == 0);
    CHECK(vps_embedded_sqlite_scan_column(scan, 4U, &value) ==
              VPS_EMBEDDED_SQLITE_OK &&
          value.kind == VPS_EMBEDDED_VALUE_BLOB &&
          value.length == sizeof(blob_value) &&
          memcmp(value.bytes, blob_value, sizeof(blob_value)) == 0);
    CHECK(vps_embedded_sqlite_scan_step(scan, &has_row) ==
              VPS_EMBEDDED_SQLITE_OK && !has_row);
    CHECK(vps_embedded_sqlite_scan_close(&scan) == VPS_EMBEDDED_SQLITE_OK);
    CHECK(vps_embedded_sqlite_close(&database) == VPS_EMBEDDED_SQLITE_OK);
    CHECK(database == NULL);
    CHECK(vps_embedded_sqlite_close(&database) == VPS_EMBEDDED_SQLITE_OK);
    CHECK(vps_fault_allocator_init(&fault, &allocator, 1U) == VPS_MEMORY_OK);
    CHECK(vps_fault_allocator_make(&fault, &fault_allocator) == VPS_MEMORY_OK);
    options.allocator = fault_allocator;
    CHECK(vps_embedded_sqlite_open(&options, &database) ==
          VPS_EMBEDDED_SQLITE_OUT_OF_MEMORY);
    CHECK(database == NULL && fault.active_allocations == 0U);
    (void)printf("[embedded-sqlite] version=%s options=%llu status=passed\n",
                 vps_embedded_sqlite_version(),
                 (unsigned long long)
                     vps_embedded_sqlite_compile_options_fingerprint());
    return 0;
}
