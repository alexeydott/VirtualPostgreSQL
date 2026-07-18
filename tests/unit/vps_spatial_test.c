#include "vps_spatial.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    (void)fprintf(stderr, "spatial_test=failed line=%d\n", __LINE__); \
    return 0; } } while (0)

typedef struct TestRows {
    const char *values[2][15];
    size_t rows;
} TestRows;

static int is_null(void *context, size_t row, size_t field)
{
    return ((TestRows *)context)->values[row][field] == NULL;
}

static const void *value(void *context, size_t row, size_t field)
{
    return ((TestRows *)context)->values[row][field];
}

static size_t length(void *context, size_t row, size_t field)
{
    const char *text = ((TestRows *)context)->values[row][field];
    return text == NULL ? 0U : strlen(text);
}

static int copy_rows(VpsMetadataRowSet *rows, TestRows *source)
{
    VpsMetadataInput input;
    input.context = source;
    input.row_count = source->rows;
    input.field_count = 15U;
    input.is_null = is_null;
    input.value = value;
    input.length = length;
    return vps_metadata_rowset_copy(rows, VPS_CATALOG_QUERY_POSTGIS,
                                    &input) == VPS_METADATA_OK;
}

static int test_discovery(void)
{
    static const char *present[15] = {
        "3.4.2", "2200", "postgis", "17000", "17001",
        "t", "t", "t", "t", "t", "t", "t", "t", "t", "t"};
    VpsAllocator allocator;
    VpsAllocator failing_allocator;
    VpsFaultAllocator fault;
    VpsMetadataRowSet rows;
    VpsSpatialCapabilities capabilities;
    VpsSpatialCapabilities failing_capabilities;
    TestRows source;
    size_t field;
    (void)memset(&source, 0, sizeof(source));
    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
          vps_metadata_rowset_init(&rows, &allocator, NULL) == VPS_METADATA_OK &&
          vps_spatial_capabilities_init(&capabilities, &allocator, NULL) ==
              VPS_SPATIAL_OK);
    source.rows = 1U;
    for (field = 0U; field < 15U; ++field)
        source.values[0][field] = present[field];
    source.values[0][5] = "true";
    source.values[0][6] = "false";
    CHECK(copy_rows(&rows, &source) &&
          vps_spatial_capabilities_resolve(&capabilities, &rows) ==
              VPS_SPATIAL_OK && capabilities.present &&
          capabilities.namespace_oid == 2200U &&
          capabilities.geometry_oid == 17000U &&
          capabilities.geography_oid == 17001U &&
          capabilities.schema.size == 7U &&
          memcmp(capabilities.schema.data, "postgis", 7U) == 0 &&
          vps_spatial_capabilities_support(
              &capabilities, VPS_SPATIAL_CAP_AS_TEXT |
                                 VPS_SPATIAL_CAP_FROM_EWKB) &&
          !vps_spatial_capabilities_support(
              &capabilities, VPS_SPATIAL_CAP_FROM_TEXT));
    source.values[0][3] = "0";
    CHECK(copy_rows(&rows, &source) &&
          vps_spatial_capabilities_resolve(&capabilities, &rows) ==
              VPS_SPATIAL_INVALID_RESULT && capabilities.present);
    source.rows = 0U;
    CHECK(copy_rows(&rows, &source) &&
          vps_spatial_capabilities_resolve(&capabilities, &rows) ==
              VPS_SPATIAL_NOT_AVAILABLE && !capabilities.present);
    source.rows = 1U;
    source.values[0][3] = "17000";
    CHECK(copy_rows(&rows, &source) &&
          vps_fault_allocator_init(&fault, &allocator, 1U) == VPS_MEMORY_OK &&
          vps_fault_allocator_make(&fault, &failing_allocator) ==
              VPS_MEMORY_OK &&
          vps_spatial_capabilities_init(&failing_capabilities,
                                        &failing_allocator, NULL) ==
              VPS_SPATIAL_OK &&
          vps_spatial_capabilities_resolve(&failing_capabilities, &rows) ==
              VPS_SPATIAL_OUT_OF_MEMORY);
    vps_spatial_capabilities_reset(&failing_capabilities);
    CHECK(vps_fault_allocator_reset(&fault, 0U) == VPS_MEMORY_OK);
    vps_spatial_capabilities_reset(&capabilities);
    vps_spatial_capabilities_reset(&capabilities);
    vps_metadata_rowset_reset(&rows);
    return 1;
}

static int test_expressions(void)
{
    VpsAllocator allocator;
    VpsSpatialCapabilities capabilities;
    VpsSpatialExpression expression;
    static const unsigned char schema[] = "geo custom";
    CHECK(vps_allocator_system(&allocator) == VPS_MEMORY_OK &&
          vps_spatial_capabilities_init(&capabilities, &allocator, NULL) ==
              VPS_SPATIAL_OK &&
          vps_buffer_append(&capabilities.schema, schema,
                            sizeof(schema) - 1U) == VPS_MEMORY_OK);
    capabilities.present = 1;
    capabilities.flags = UINT32_MAX;
    CHECK(vps_spatial_read_expression(
              &capabilities, VPS_SPATIAL_KIND_GEOMETRY,
              VPS_SPATIAL_FORMAT_WKT, "shape\"name", 10U, &expression) ==
              VPS_SPATIAL_OK &&
          strcmp(expression.sql,
                 "\"geo custom\".ST_AsText(\"shape\"\"name\")") == 0 &&
          !expression.binary_result);
    CHECK(vps_spatial_read_expression(
              &capabilities, VPS_SPATIAL_KIND_GEOGRAPHY,
              VPS_SPATIAL_FORMAT_WKB, "position", 8U, &expression) ==
              VPS_SPATIAL_OK && strstr(expression.sql, "'NDR'") != NULL &&
          expression.binary_result);
    CHECK(vps_spatial_write_expression(
              &capabilities, VPS_SPATIAL_KIND_GEOMETRY,
              VPS_SPATIAL_FORMAT_WKT, 2U, 3U, &expression) ==
              VPS_SPATIAL_OK &&
          strcmp(expression.sql,
                 "\"geo custom\".ST_GeomFromText($2::pg_catalog.text,$3::pg_catalog.int4)") == 0 &&
          expression.parameter_count == 2U);
    CHECK(vps_spatial_write_expression(
              &capabilities, VPS_SPATIAL_KIND_GEOGRAPHY,
              VPS_SPATIAL_FORMAT_EWKB, 4U, 0U, &expression) ==
              VPS_SPATIAL_OK && strstr(expression.sql, "ST_GeogFromWKB") != NULL);
    CHECK(vps_spatial_read_expression(
              &capabilities, VPS_SPATIAL_KIND_GEOMETRY,
              VPS_SPATIAL_FORMAT_SPATIALITE, "shape", 5U, &expression) ==
              VPS_SPATIAL_NOT_AVAILABLE);
    vps_spatial_capabilities_reset(&capabilities);
    return 1;
}

static int test_wkt_validation(void)
{
    static const char *valid[] = {
        "POINT (1 2)",
        "LINESTRING (0 0,1 1)",
        "POLYGON ((0 0,0 1,1 1,0 0))",
        "MULTIPOINT ((0 0),(1 1))",
        "MULTILINESTRING ((0 0,1 1),(2 2,3 3))",
        "MULTIPOLYGON (((0 0,0 1,1 1,0 0)))",
        "GEOMETRYCOLLECTION (POINT (1 2),LINESTRING (0 0,1 1))",
        "POINT EMPTY", "POINT Z (1 2 3)", "POINT M (1 2 3)",
        "POINT ZM (1 2 3 4)"};
    VpsSpatialValidation validation;
    VpsSpatialLimits limits = {1024U, 100U, 8U};
    size_t index;
    for (index = 0U; index < sizeof(valid) / sizeof(valid[0]); ++index)
        CHECK(vps_spatial_validate_text(
                  valid[index], strlen(valid[index]), VPS_SPATIAL_FORMAT_WKT,
                  VPS_SPATIAL_TYPE_ANY, 4326U, &limits, &validation) ==
                  VPS_SPATIAL_OK);
    CHECK(vps_spatial_validate_text(
              "SRID=4326;POINT (1 2)", strlen("SRID=4326;POINT (1 2)"), VPS_SPATIAL_FORMAT_EWKT,
              VPS_SPATIAL_TYPE_POINT, 4326U, &limits, &validation) ==
              VPS_SPATIAL_OK && validation.has_srid &&
          validation.srid == 4326U);
    CHECK(vps_spatial_validate_text(
              "SRID=3857;POINT (1 2)", strlen("SRID=3857;POINT (1 2)"), VPS_SPATIAL_FORMAT_EWKT,
              VPS_SPATIAL_TYPE_POINT, 4326U, &limits, &validation) ==
              VPS_SPATIAL_SRID_MISMATCH);
    CHECK(vps_spatial_validate_text(
              "POINT (nan 1)", strlen("POINT (nan 1)"), VPS_SPATIAL_FORMAT_WKT,
              VPS_SPATIAL_TYPE_POINT, 0U, &limits, &validation) ==
              VPS_SPATIAL_MALFORMED);
    CHECK(vps_spatial_validate_text(
              "POINT (1 2) junk", strlen("POINT (1 2) junk"), VPS_SPATIAL_FORMAT_WKT,
              VPS_SPATIAL_TYPE_POINT, 0U, &limits, &validation) ==
              VPS_SPATIAL_MALFORMED);
    limits.max_points = 1U;
    CHECK(vps_spatial_validate_text(
              "LINESTRING (0 0,1 1)", strlen("LINESTRING (0 0,1 1)"), VPS_SPATIAL_FORMAT_WKT,
              VPS_SPATIAL_TYPE_LINESTRING, 0U, &limits, &validation) ==
              VPS_SPATIAL_LIMIT_EXCEEDED);
    return 1;
}

static int test_wkb_validation(void)
{
    static const unsigned char point[] = {
        1U,1U,0U,0U,0U,
        0U,0U,0U,0U,0U,0U,0xf0U,0x3fU,
        0U,0U,0U,0U,0U,0U,0x00U,0x40U};
    static const unsigned char ewkb[] = {
        1U,1U,0U,0U,0x20U,0xe6U,0x10U,0U,0U,
        0U,0U,0U,0U,0U,0U,0xf0U,0x3fU,
        0U,0U,0U,0U,0U,0U,0x00U,0x40U};
    static const unsigned char postgis_point_z[] = {
        1U,0xe9U,3U,0U,0U,0U,0U,0U,0U,0U,0U,0x3eU,0x40U,
        0U,0U,0U,0U,0U,0U,0x24U,0x40U,
        0U,0U,0U,0U,0U,0U,0x14U,0x40U};
    static const unsigned char postgis_geography_point[] = {
        1U,1U,0U,0U,0U,0x3cU,0xdbU,0xa3U,0x37U,0xdcU,0xc3U,
        0x51U,0xc0U,0x6dU,0x37U,0xc1U,0x37U,0x4dU,0x37U,0x48U,0x40U};
    unsigned char trailing[sizeof(point) + 1U];
    VpsSpatialValidation validation;
    VpsSpatialLimits limits = {1024U, 100U, 8U};
    CHECK(vps_spatial_validate_binary(
              point, sizeof(point), VPS_SPATIAL_FORMAT_WKB,
              VPS_SPATIAL_TYPE_POINT, 4326U, &limits, &validation) ==
              VPS_SPATIAL_OK && validation.point_count == 1U &&
          validation.srid == 4326U);
    CHECK(vps_spatial_validate_binary(
              ewkb, sizeof(ewkb), VPS_SPATIAL_FORMAT_EWKB,
              VPS_SPATIAL_TYPE_POINT, 4326U, &limits, &validation) ==
              VPS_SPATIAL_OK && validation.has_srid);
    CHECK(vps_spatial_validate_binary(
              ewkb, sizeof(ewkb), VPS_SPATIAL_FORMAT_EWKB,
              VPS_SPATIAL_TYPE_POINT, 3857U, &limits, &validation) ==
              VPS_SPATIAL_SRID_MISMATCH);
    CHECK(vps_spatial_validate_binary(
              postgis_point_z, sizeof(postgis_point_z),
              VPS_SPATIAL_FORMAT_WKB, VPS_SPATIAL_TYPE_POINT, 4326U,
              &limits, &validation) == VPS_SPATIAL_OK &&
          validation.dimensions == 3U);
    CHECK(vps_spatial_validate_binary(
              postgis_geography_point, sizeof(postgis_geography_point),
              VPS_SPATIAL_FORMAT_WKB, VPS_SPATIAL_TYPE_POINT, 4326U,
              &limits, &validation) == VPS_SPATIAL_OK &&
          validation.dimensions == 2U);
    CHECK(vps_spatial_validate_binary(
              point, sizeof(point) - 1U, VPS_SPATIAL_FORMAT_WKB,
              VPS_SPATIAL_TYPE_POINT, 0U, &limits, &validation) ==
              VPS_SPATIAL_MALFORMED);
    (void)memcpy(trailing, point, sizeof(point));
    trailing[sizeof(point)] = 0U;
    CHECK(vps_spatial_validate_binary(
              trailing, sizeof(trailing), VPS_SPATIAL_FORMAT_WKB,
              VPS_SPATIAL_TYPE_POINT, 0U, &limits, &validation) ==
              VPS_SPATIAL_MALFORMED);
    return 1;
}

static int test_typmod(void)
{
    VpsSpatialGeometryType type;
    uint32_t dimensions;
    uint32_t srid;
    CHECK(vps_spatial_typmod_decode(1107462, &type, &dimensions, &srid) ==
              VPS_SPATIAL_OK && type == VPS_SPATIAL_TYPE_POINT &&
          dimensions == 3U && srid == 4326U);
    CHECK(vps_spatial_typmod_decode(-1, &type, &dimensions, &srid) ==
              VPS_SPATIAL_OK && type == VPS_SPATIAL_TYPE_ANY &&
          dimensions == 0U && srid == 0U);
    return 1;
}

int main(void)
{
    VpsCatalogQuerySpec spec;
    if (vps_metadata_catalog_query_spec(VPS_CATALOG_QUERY_POSTGIS, &spec) !=
            VPS_METADATA_OK || spec.parameter_count != 1U ||
        spec.result_field_count != 15U ||
        strstr(spec.sql, "pg_catalog.pg_extension") == NULL ||
        strstr(spec.sql, "pg_catalog.pg_depend") == NULL ||
        !test_discovery() || !test_expressions() || !test_wkt_validation() ||
        !test_wkb_validation() || !test_typmod()) return 1;
    (void)printf("spatial_tests=passed\n");
    return 0;
}
