#ifndef VPS_SPATIAL_H
#define VPS_SPATIAL_H

#include "vps_metadata.h"

#include <stdint.h>

#define VPS_SPATIAL_SCHEMA_MAX_BYTES 63U
#define VPS_SPATIAL_VERSION_MAX_BYTES 63U
#define VPS_SPATIAL_EXPRESSION_MAX_BYTES 511U
#define VPS_SPATIAL_DEFAULT_MAX_BYTES (16U * 1024U * 1024U)
#define VPS_SPATIAL_DEFAULT_MAX_POINTS 1000000U
#define VPS_SPATIAL_DEFAULT_MAX_DEPTH 64U

#define VPS_SPATIAL_CAP_AS_TEXT       (UINT32_C(1) << 0)
#define VPS_SPATIAL_CAP_FROM_TEXT     (UINT32_C(1) << 1)
#define VPS_SPATIAL_CAP_AS_BINARY     (UINT32_C(1) << 2)
#define VPS_SPATIAL_CAP_FROM_WKB      (UINT32_C(1) << 3)
#define VPS_SPATIAL_CAP_AS_EWKT       (UINT32_C(1) << 4)
#define VPS_SPATIAL_CAP_FROM_EWKT     (UINT32_C(1) << 5)
#define VPS_SPATIAL_CAP_AS_EWKB       (UINT32_C(1) << 6)
#define VPS_SPATIAL_CAP_FROM_EWKB     (UINT32_C(1) << 7)
#define VPS_SPATIAL_CAP_GEOG_TEXT     (UINT32_C(1) << 8)
#define VPS_SPATIAL_CAP_GEOG_WKB      (UINT32_C(1) << 9)

typedef enum VpsSpatialResult {
    VPS_SPATIAL_OK = 0,
    VPS_SPATIAL_INVALID_ARGUMENT = 1,
    VPS_SPATIAL_INVALID_RESULT = 2,
    VPS_SPATIAL_NOT_AVAILABLE = 3,
    VPS_SPATIAL_UNSUPPORTED = 4,
    VPS_SPATIAL_MALFORMED = 5,
    VPS_SPATIAL_LIMIT_EXCEEDED = 6,
    VPS_SPATIAL_SRID_MISMATCH = 7,
    VPS_SPATIAL_OUT_OF_MEMORY = 8
} VpsSpatialResult;

typedef enum VpsSpatialFormat {
    VPS_SPATIAL_FORMAT_WKT = 0,
    VPS_SPATIAL_FORMAT_WKB = 1,
    VPS_SPATIAL_FORMAT_EWKT = 2,
    VPS_SPATIAL_FORMAT_EWKB = 3,
    VPS_SPATIAL_FORMAT_NONE = 4,
    VPS_SPATIAL_FORMAT_SPATIALITE = 5
} VpsSpatialFormat;

typedef enum VpsSpatialKind {
    VPS_SPATIAL_KIND_GEOMETRY = 0,
    VPS_SPATIAL_KIND_GEOGRAPHY = 1
} VpsSpatialKind;

typedef enum VpsSpatialGeometryType {
    VPS_SPATIAL_TYPE_ANY = 0,
    VPS_SPATIAL_TYPE_POINT = 1,
    VPS_SPATIAL_TYPE_LINESTRING = 2,
    VPS_SPATIAL_TYPE_POLYGON = 3,
    VPS_SPATIAL_TYPE_MULTIPOINT = 4,
    VPS_SPATIAL_TYPE_MULTILINESTRING = 5,
    VPS_SPATIAL_TYPE_MULTIPOLYGON = 6,
    VPS_SPATIAL_TYPE_GEOMETRYCOLLECTION = 7
} VpsSpatialGeometryType;

typedef struct VpsSpatialLimits {
    size_t max_bytes;
    uint32_t max_points;
    uint32_t max_depth;
} VpsSpatialLimits;

typedef struct VpsSpatialValidation {
    VpsSpatialGeometryType type;
    uint32_t dimensions;
    uint32_t srid;
    uint32_t point_count;
    size_t error_offset;
    int has_srid;
    int empty;
} VpsSpatialValidation;

typedef struct VpsSpatialExpression {
    char sql[VPS_SPATIAL_EXPRESSION_MAX_BYTES + 1U];
    size_t length;
    uint32_t parameter_count;
    int binary_result;
} VpsSpatialExpression;

/* Owns schema/version through copied allocator. Mutable access is
 * caller-serialized; reset is idempotent and preserves no server handles. */
typedef struct VpsSpatialCapabilities {
    VpsAllocator allocator;
    VpsBuffer schema;
    VpsBuffer version;
    uint32_t namespace_oid;
    uint32_t geometry_oid;
    uint32_t geography_oid;
    uint32_t flags;
    VpsLogger *logger;
    int present;
    int initialized;
} VpsSpatialCapabilities;

VpsSpatialResult vps_spatial_capabilities_init(
    VpsSpatialCapabilities *capabilities,
    const VpsAllocator *allocator,
    VpsLogger *logger);
VpsSpatialResult vps_spatial_capabilities_resolve(
    VpsSpatialCapabilities *capabilities,
    const VpsMetadataRowSet *rowset);
int vps_spatial_capabilities_support(
    const VpsSpatialCapabilities *capabilities,
    uint32_t required_flags);
VpsSpatialResult vps_spatial_read_expression(
    const VpsSpatialCapabilities *capabilities,
    VpsSpatialKind kind,
    VpsSpatialFormat format,
    const char *column_name,
    size_t column_name_length,
    VpsSpatialExpression *expression);
VpsSpatialResult vps_spatial_write_expression(
    const VpsSpatialCapabilities *capabilities,
    VpsSpatialKind kind,
    VpsSpatialFormat format,
    uint32_t value_parameter,
    uint32_t srid_parameter,
    VpsSpatialExpression *expression);
VpsSpatialResult vps_spatial_validate_text(
    const char *text,
    size_t length,
    VpsSpatialFormat format,
    VpsSpatialGeometryType expected_type,
    uint32_t expected_srid,
    const VpsSpatialLimits *limits,
    VpsSpatialValidation *validation);
VpsSpatialResult vps_spatial_validate_binary(
    const void *bytes,
    size_t length,
    VpsSpatialFormat format,
    VpsSpatialGeometryType expected_type,
    uint32_t expected_srid,
    const VpsSpatialLimits *limits,
    VpsSpatialValidation *validation);
void vps_spatial_capabilities_reset(VpsSpatialCapabilities *capabilities);
const char *vps_spatial_result_name(VpsSpatialResult result);
const char *vps_spatial_format_name(VpsSpatialFormat format);
VpsSpatialResult vps_spatial_typmod_decode(
    int32_t type_modifier,
    VpsSpatialGeometryType *type,
    uint32_t *dimensions,
    uint32_t *srid);

#endif
