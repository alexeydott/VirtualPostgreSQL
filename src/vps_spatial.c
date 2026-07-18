#include "vps_spatial.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VpsSpatialResult vps_spatial_cell(
    const VpsMetadataRowSet *rows, size_t field,
    const unsigned char **value, size_t *length)
{
    int is_null = 0;
    if (vps_metadata_rowset_cell(rows, 0U, field, value, length, &is_null) !=
            VPS_METADATA_OK || is_null || *value == NULL)
        return VPS_SPATIAL_INVALID_RESULT;
    return VPS_SPATIAL_OK;
}

static VpsSpatialResult vps_spatial_uint32(
    const VpsMetadataRowSet *rows, size_t field, uint32_t *result)
{
    const unsigned char *value;
    size_t length;
    uint64_t parsed = 0U;
    size_t index;
    if (result == NULL || vps_spatial_cell(rows, field, &value, &length) !=
                              VPS_SPATIAL_OK || length == 0U || length > 10U)
        return VPS_SPATIAL_INVALID_RESULT;
    for (index = 0U; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9')
            return VPS_SPATIAL_INVALID_RESULT;
        parsed = parsed * UINT64_C(10) + (uint64_t)(value[index] - '0');
        if (parsed > UINT32_MAX) return VPS_SPATIAL_INVALID_RESULT;
    }
    *result = (uint32_t)parsed;
    return VPS_SPATIAL_OK;
}

static VpsSpatialResult vps_spatial_boolean(
    const VpsMetadataRowSet *rows, size_t field, int *result)
{
    const unsigned char *value;
    size_t length;
    if (result == NULL || vps_spatial_cell(rows, field, &value, &length) !=
                              VPS_SPATIAL_OK ||
        !((length == 1U && (value[0] == 't' || value[0] == 'f')) ||
          (length == 4U && memcmp(value, "true", 4U) == 0) ||
          (length == 5U && memcmp(value, "false", 5U) == 0)))
        return VPS_SPATIAL_INVALID_RESULT;
    *result = value[0] == 't';
    return VPS_SPATIAL_OK;
}

static void vps_spatial_log(const VpsSpatialCapabilities *capabilities,
                            const char *status)
{
    static const char operation[] = "postgis_discovery";
    VpsLogEvent event;
    if (capabilities == NULL || capabilities->logger == NULL ||
        vps_log_event_init(&event, strcmp(status, "passed") == 0
                                      ? VPS_LOG_LEVEL_INFO
                                      : VPS_LOG_LEVEL_WARN) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION, operation,
                                 sizeof(operation) - 1U) != VPS_LOG_OK ||
        vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS, status,
                                 strlen(status)) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_NAMESPACE_OID,
                                 capabilities->namespace_oid) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_TYPE_OID,
                                 capabilities->geometry_oid) != VPS_LOG_OK ||
        vps_log_event_add_uint64(&event, VPS_LOG_FIELD_FLAGS,
                                 capabilities->flags) != VPS_LOG_OK) return;
    vps_logger_emit(capabilities->logger, &event);
}

VpsSpatialResult vps_spatial_capabilities_init(
    VpsSpatialCapabilities *capabilities, const VpsAllocator *allocator,
    VpsLogger *logger)
{
    if (capabilities == NULL || !vps_allocator_is_valid(allocator))
        return VPS_SPATIAL_INVALID_ARGUMENT;
    (void)memset(capabilities, 0, sizeof(*capabilities));
    capabilities->allocator = *allocator;
    capabilities->logger = logger;
    if (vps_buffer_init(&capabilities->schema, allocator,
                        VPS_SPATIAL_SCHEMA_MAX_BYTES + 1U) != VPS_MEMORY_OK ||
        vps_buffer_init(&capabilities->version, allocator,
                        VPS_SPATIAL_VERSION_MAX_BYTES + 1U) != VPS_MEMORY_OK) {
        vps_spatial_capabilities_reset(capabilities);
        return VPS_SPATIAL_OUT_OF_MEMORY;
    }
    capabilities->initialized = 1;
    return VPS_SPATIAL_OK;
}

VpsSpatialResult vps_spatial_capabilities_resolve(
    VpsSpatialCapabilities *capabilities, const VpsMetadataRowSet *rows)
{
    const unsigned char *version;
    const unsigned char *schema;
    size_t version_length;
    size_t schema_length;
    uint32_t flags = 0U;
    uint32_t namespace_oid;
    uint32_t geometry_oid;
    uint32_t geography_oid;
    size_t field;
    if (capabilities == NULL || !capabilities->initialized || rows == NULL ||
        !rows->initialized || rows->field_count != 15U || rows->row_count > 1U)
        return VPS_SPATIAL_INVALID_ARGUMENT;
    if (rows->row_count == 0U) {
        vps_buffer_reset(&capabilities->schema);
        vps_buffer_reset(&capabilities->version);
        capabilities->present = 0;
        capabilities->namespace_oid = 0U;
        capabilities->geometry_oid = 0U;
        capabilities->geography_oid = 0U;
        capabilities->flags = 0U;
        capabilities->initialized = 1;
        vps_spatial_log(capabilities, "absent");
        return VPS_SPATIAL_NOT_AVAILABLE;
    }
    if (vps_spatial_cell(rows, 0U, &version, &version_length) != VPS_SPATIAL_OK ||
        vps_spatial_cell(rows, 2U, &schema, &schema_length) != VPS_SPATIAL_OK ||
        version_length == 0U || version_length > VPS_SPATIAL_VERSION_MAX_BYTES ||
        schema_length == 0U || schema_length > VPS_SPATIAL_SCHEMA_MAX_BYTES ||
        memchr(schema, '\0', schema_length) != NULL ||
        vps_spatial_uint32(rows, 1U, &namespace_oid) != VPS_SPATIAL_OK ||
        vps_spatial_uint32(rows, 3U, &geometry_oid) != VPS_SPATIAL_OK ||
        vps_spatial_uint32(rows, 4U, &geography_oid) != VPS_SPATIAL_OK ||
        namespace_oid == 0U || geometry_oid == 0U || geography_oid == 0U)
        return VPS_SPATIAL_INVALID_RESULT;
    for (field = 5U; field < 15U; ++field) {
        int available;
        if (vps_spatial_boolean(rows, field, &available) != VPS_SPATIAL_OK)
            return VPS_SPATIAL_INVALID_RESULT;
        if (available) flags |= UINT32_C(1) << (field - 5U);
    }
    vps_buffer_reset(&capabilities->schema);
    vps_buffer_reset(&capabilities->version);
    capabilities->initialized = 1;
    if (vps_buffer_append(&capabilities->schema, schema, schema_length) !=
            VPS_MEMORY_OK ||
        vps_buffer_append(&capabilities->version, version, version_length) !=
            VPS_MEMORY_OK) {
        vps_buffer_reset(&capabilities->schema);
        vps_buffer_reset(&capabilities->version);
        capabilities->initialized = 1;
        return VPS_SPATIAL_OUT_OF_MEMORY;
    }
    capabilities->namespace_oid = namespace_oid;
    capabilities->geometry_oid = geometry_oid;
    capabilities->geography_oid = geography_oid;
    capabilities->flags = flags;
    capabilities->present = 1;
    vps_spatial_log(capabilities, "passed");
    return VPS_SPATIAL_OK;
}

int vps_spatial_capabilities_support(
    const VpsSpatialCapabilities *capabilities, uint32_t required_flags)
{
    return capabilities != NULL && capabilities->initialized &&
           capabilities->present &&
           (capabilities->flags & required_flags) == required_flags;
}

static int vps_spatial_identifier(char *output, size_t capacity,
                                  const unsigned char *input, size_t length,
                                  size_t *written)
{
    size_t source;
    size_t target = 0U;
    if (output == NULL || input == NULL || written == NULL || length == 0U ||
        memchr(input, '\0', length) != NULL || capacity < 3U) return 0;
    output[target++] = '"';
    for (source = 0U; source < length; ++source) {
        if (target + (input[source] == '"' ? 2U : 1U) + 1U >= capacity)
            return 0;
        if (input[source] == '"') output[target++] = '"';
        output[target++] = (char)input[source];
    }
    output[target++] = '"';
    output[target] = '\0';
    *written = target;
    return 1;
}

static VpsSpatialResult vps_spatial_qualified_parts(
    const VpsSpatialCapabilities *capabilities,
    const char *column, size_t column_length,
    char *schema_sql, size_t schema_capacity,
    char *column_sql, size_t column_capacity)
{
    size_t ignored;
    if (capabilities == NULL || !capabilities->initialized ||
        !capabilities->present || schema_sql == NULL ||
        !vps_spatial_identifier(schema_sql, schema_capacity,
                                capabilities->schema.data,
                                capabilities->schema.size, &ignored))
        return VPS_SPATIAL_NOT_AVAILABLE;
    if (column != NULL &&
        !vps_spatial_identifier(column_sql, column_capacity,
                                (const unsigned char *)column,
                                column_length, &ignored))
        return VPS_SPATIAL_INVALID_ARGUMENT;
    return VPS_SPATIAL_OK;
}

static uint32_t vps_spatial_required_capability(VpsSpatialFormat format,
                                                int write,
                                                VpsSpatialKind kind)
{
    if (kind == VPS_SPATIAL_KIND_GEOGRAPHY && write) {
        if (format == VPS_SPATIAL_FORMAT_EWKT)
            return VPS_SPATIAL_CAP_GEOG_TEXT;
        if (format == VPS_SPATIAL_FORMAT_EWKB)
            return VPS_SPATIAL_CAP_GEOG_WKB;
    }
    switch (format) {
        case VPS_SPATIAL_FORMAT_WKT:
            return write ? VPS_SPATIAL_CAP_FROM_TEXT : VPS_SPATIAL_CAP_AS_TEXT;
        case VPS_SPATIAL_FORMAT_WKB:
            return write ? VPS_SPATIAL_CAP_FROM_WKB : VPS_SPATIAL_CAP_AS_BINARY;
        case VPS_SPATIAL_FORMAT_EWKT:
            return write ? VPS_SPATIAL_CAP_FROM_EWKT : VPS_SPATIAL_CAP_AS_EWKT;
        case VPS_SPATIAL_FORMAT_EWKB:
            return write ? VPS_SPATIAL_CAP_FROM_EWKB : VPS_SPATIAL_CAP_AS_EWKB;
        default: return 0U;
    }
}

VpsSpatialResult vps_spatial_read_expression(
    const VpsSpatialCapabilities *capabilities, VpsSpatialKind kind,
    VpsSpatialFormat format, const char *column_name,
    size_t column_name_length, VpsSpatialExpression *expression)
{
    char schema[2U * VPS_SPATIAL_SCHEMA_MAX_BYTES + 3U];
    char column[2U * VPS_METADATA_NAME_MAX_BYTES + 3U];
    const char *function_name;
    uint32_t required;
    int written;
    if (expression == NULL ||
        (kind != VPS_SPATIAL_KIND_GEOMETRY &&
         kind != VPS_SPATIAL_KIND_GEOGRAPHY))
        return VPS_SPATIAL_INVALID_ARGUMENT;
    (void)memset(expression, 0, sizeof(*expression));
    if (format == VPS_SPATIAL_FORMAT_NONE) return VPS_SPATIAL_UNSUPPORTED;
    if (format == VPS_SPATIAL_FORMAT_SPATIALITE)
        return VPS_SPATIAL_NOT_AVAILABLE;
    required = vps_spatial_required_capability(format, 0, kind);
    if (!vps_spatial_capabilities_support(capabilities, required))
        return VPS_SPATIAL_UNSUPPORTED;
    if (vps_spatial_qualified_parts(capabilities, column_name,
                                    column_name_length, schema,
                                    sizeof(schema), column,
                                    sizeof(column)) != VPS_SPATIAL_OK)
        return VPS_SPATIAL_INVALID_ARGUMENT;
    function_name = format == VPS_SPATIAL_FORMAT_WKT ? "ST_AsText"
                    : format == VPS_SPATIAL_FORMAT_WKB ? "ST_AsBinary"
                    : format == VPS_SPATIAL_FORMAT_EWKT ? "ST_AsEWKT"
                                                       : "ST_AsEWKB";
    written = kind == VPS_SPATIAL_KIND_GEOGRAPHY &&
                      (format == VPS_SPATIAL_FORMAT_EWKT ||
                       format == VPS_SPATIAL_FORMAT_EWKB)
                  ? snprintf(expression->sql, sizeof(expression->sql),
                             "%s.%s(%s::%s.\"geometry\")", schema,
                             function_name, column, schema)
              : format == VPS_SPATIAL_FORMAT_WKB
                  ? snprintf(expression->sql, sizeof(expression->sql),
                             "%s.%s(%s,'NDR')", schema, function_name, column)
                  : snprintf(expression->sql, sizeof(expression->sql),
                             "%s.%s(%s)", schema, function_name, column);
    if (written <= 0 || (size_t)written >= sizeof(expression->sql))
        return VPS_SPATIAL_LIMIT_EXCEEDED;
    expression->length = (size_t)written;
    expression->binary_result = format == VPS_SPATIAL_FORMAT_WKB ||
                                format == VPS_SPATIAL_FORMAT_EWKB;
    return VPS_SPATIAL_OK;
}

VpsSpatialResult vps_spatial_write_expression(
    const VpsSpatialCapabilities *capabilities, VpsSpatialKind kind,
    VpsSpatialFormat format, uint32_t value_parameter,
    uint32_t srid_parameter, VpsSpatialExpression *expression)
{
    char schema[2U * VPS_SPATIAL_SCHEMA_MAX_BYTES + 3U];
    uint32_t required;
    int written;
    if (expression == NULL || value_parameter == 0U ||
        value_parameter > 65535U || srid_parameter > 65535U ||
        (kind != VPS_SPATIAL_KIND_GEOMETRY &&
         kind != VPS_SPATIAL_KIND_GEOGRAPHY))
        return VPS_SPATIAL_INVALID_ARGUMENT;
    (void)memset(expression, 0, sizeof(*expression));
    if (format == VPS_SPATIAL_FORMAT_NONE) return VPS_SPATIAL_UNSUPPORTED;
    if (format == VPS_SPATIAL_FORMAT_SPATIALITE)
        return VPS_SPATIAL_NOT_AVAILABLE;
    required = vps_spatial_required_capability(format, 1, kind);
    if (!vps_spatial_capabilities_support(capabilities, required))
        return VPS_SPATIAL_UNSUPPORTED;
    if (vps_spatial_qualified_parts(capabilities, NULL, 0U, schema,
                                    sizeof(schema), NULL, 0U) != VPS_SPATIAL_OK)
        return VPS_SPATIAL_NOT_AVAILABLE;
    if (kind == VPS_SPATIAL_KIND_GEOGRAPHY &&
        (format == VPS_SPATIAL_FORMAT_WKT ||
         format == VPS_SPATIAL_FORMAT_WKB)) {
        if (srid_parameter == 0U) return VPS_SPATIAL_INVALID_ARGUMENT;
        written = snprintf(
            expression->sql, sizeof(expression->sql),
            "CAST(%s.%s($%u::pg_catalog.%s,$%u::pg_catalog.int4) AS %s.\"geography\")",
            schema,
            format == VPS_SPATIAL_FORMAT_WKT
                ? "ST_GeomFromText" : "ST_GeomFromWKB",
            value_parameter,
            format == VPS_SPATIAL_FORMAT_WKT ? "text" : "bytea",
            srid_parameter, schema);
    } else if (kind == VPS_SPATIAL_KIND_GEOGRAPHY) {
        const char *function_name =
            (format == VPS_SPATIAL_FORMAT_WKT ||
             format == VPS_SPATIAL_FORMAT_EWKT)
                ? "ST_GeogFromText" : "ST_GeogFromWKB";
        written = snprintf(expression->sql, sizeof(expression->sql),
                           "%s.%s($%u::pg_catalog.%s)", schema,
                           function_name, value_parameter,
                           (format == VPS_SPATIAL_FORMAT_WKT ||
                            format == VPS_SPATIAL_FORMAT_EWKT)
                               ? "text" : "bytea");
    } else if (format == VPS_SPATIAL_FORMAT_WKT ||
               format == VPS_SPATIAL_FORMAT_WKB) {
        if (srid_parameter == 0U) return VPS_SPATIAL_INVALID_ARGUMENT;
        written = snprintf(expression->sql, sizeof(expression->sql),
                           "%s.%s($%u::pg_catalog.%s,$%u::pg_catalog.int4)",
                           schema,
                           format == VPS_SPATIAL_FORMAT_WKT
                               ? "ST_GeomFromText" : "ST_GeomFromWKB",
                           value_parameter,
                           format == VPS_SPATIAL_FORMAT_WKT ? "text" : "bytea",
                           srid_parameter);
    } else {
        written = snprintf(expression->sql, sizeof(expression->sql),
                           "%s.%s($%u::pg_catalog.%s)", schema,
                           format == VPS_SPATIAL_FORMAT_EWKT
                               ? "ST_GeomFromEWKT" : "ST_GeomFromEWKB",
                           value_parameter,
                           format == VPS_SPATIAL_FORMAT_EWKT ? "text" : "bytea");
    }
    if (written <= 0 || (size_t)written >= sizeof(expression->sql))
        return VPS_SPATIAL_LIMIT_EXCEEDED;
    expression->length = (size_t)written;
    expression->parameter_count = srid_parameter == 0U ? 1U : 2U;
    return VPS_SPATIAL_OK;
}

typedef enum VpsWktTokenKind {
    VPS_WKT_END = 0,
    VPS_WKT_WORD = 1,
    VPS_WKT_NUMBER = 2,
    VPS_WKT_LEFT = 3,
    VPS_WKT_RIGHT = 4,
    VPS_WKT_COMMA = 5,
    VPS_WKT_SEMICOLON = 6,
    VPS_WKT_EQUAL = 7,
    VPS_WKT_INVALID = 8
} VpsWktTokenKind;

typedef struct VpsWktParser {
    const char *text;
    size_t length;
    size_t position;
    size_t token_offset;
    size_t token_length;
    VpsWktTokenKind token;
    VpsSpatialLimits limits;
    VpsSpatialValidation result;
} VpsWktParser;

static int vps_ascii_equal(const char *text, size_t length, const char *word)
{
    size_t index;
    if (strlen(word) != length) return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char left = (unsigned char)text[index];
        unsigned char right = (unsigned char)word[index];
        if (left >= 'a' && left <= 'z') left = (unsigned char)(left - 32U);
        if (right >= 'a' && right <= 'z') right = (unsigned char)(right - 32U);
        if (left != right) return 0;
    }
    return 1;
}

static void vps_wkt_next(VpsWktParser *parser)
{
    size_t start;
    int digits = 0;
    while (parser->position < parser->length &&
           isspace((unsigned char)parser->text[parser->position]))
        parser->position += 1U;
    parser->token_offset = parser->position;
    parser->token_length = 0U;
    if (parser->position == parser->length) {
        parser->token = VPS_WKT_END;
        return;
    }
    switch (parser->text[parser->position]) {
        case '(': parser->token = VPS_WKT_LEFT; parser->position += 1U; return;
        case ')': parser->token = VPS_WKT_RIGHT; parser->position += 1U; return;
        case ',': parser->token = VPS_WKT_COMMA; parser->position += 1U; return;
        case ';': parser->token = VPS_WKT_SEMICOLON; parser->position += 1U; return;
        case '=': parser->token = VPS_WKT_EQUAL; parser->position += 1U; return;
        default: break;
    }
    start = parser->position;
    if (isalpha((unsigned char)parser->text[start])) {
        while (parser->position < parser->length &&
               isalpha((unsigned char)parser->text[parser->position]))
            parser->position += 1U;
        parser->token = VPS_WKT_WORD;
        parser->token_length = parser->position - start;
        return;
    }
    if (parser->text[parser->position] == '+' ||
        parser->text[parser->position] == '-') parser->position += 1U;
    while (parser->position < parser->length &&
           isdigit((unsigned char)parser->text[parser->position])) {
        parser->position += 1U;
        digits = 1;
    }
    if (parser->position < parser->length &&
        parser->text[parser->position] == '.') {
        parser->position += 1U;
        while (parser->position < parser->length &&
               isdigit((unsigned char)parser->text[parser->position])) {
            parser->position += 1U;
            digits = 1;
        }
    }
    if (digits && parser->position < parser->length &&
        (parser->text[parser->position] == 'e' ||
         parser->text[parser->position] == 'E')) {
        size_t exponent_start = parser->position++;
        int exponent_digits = 0;
        if (parser->position < parser->length &&
            (parser->text[parser->position] == '+' ||
             parser->text[parser->position] == '-')) parser->position += 1U;
        while (parser->position < parser->length &&
               isdigit((unsigned char)parser->text[parser->position])) {
            parser->position += 1U;
            exponent_digits = 1;
        }
        if (!exponent_digits) parser->position = exponent_start;
    }
    parser->token_length = parser->position - start;
    parser->token = digits && parser->token_length != 0U
                        ? VPS_WKT_NUMBER : VPS_WKT_INVALID;
    if (parser->token == VPS_WKT_INVALID) parser->position = start + 1U;
}

static int vps_wkt_word(const VpsWktParser *parser, const char *word)
{
    return parser->token == VPS_WKT_WORD &&
           vps_ascii_equal(parser->text + parser->token_offset,
                           parser->token_length, word);
}

static int vps_wkt_number(VpsWktParser *parser)
{
    char number[96];
    char *end = NULL;
    double parsed;
    if (parser->token != VPS_WKT_NUMBER ||
        parser->token_length >= sizeof(number)) return 0;
    (void)memcpy(number, parser->text + parser->token_offset,
                 parser->token_length);
    number[parser->token_length] = '\0';
    parsed = strtod(number, &end);
    if (end == NULL || *end != '\0' || !isfinite(parsed)) return 0;
    vps_wkt_next(parser);
    return 1;
}

static int vps_wkt_coordinate(VpsWktParser *parser, uint32_t dimensions)
{
    uint32_t index;
    for (index = 0U; index < dimensions; ++index)
        if (!vps_wkt_number(parser)) return 0;
    if (parser->result.point_count == parser->limits.max_points) return 0;
    parser->result.point_count += 1U;
    return parser->token == VPS_WKT_COMMA || parser->token == VPS_WKT_RIGHT;
}

static int vps_wkt_coordinate_list(VpsWktParser *parser,
                                   uint32_t dimensions, uint32_t minimum)
{
    uint32_t count = 0U;
    if (parser->token != VPS_WKT_LEFT) return 0;
    vps_wkt_next(parser);
    for (;;) {
        if (!vps_wkt_coordinate(parser, dimensions)) return 0;
        count += 1U;
        if (parser->token == VPS_WKT_RIGHT) {
            vps_wkt_next(parser);
            return count >= minimum;
        }
        if (parser->token != VPS_WKT_COMMA) return 0;
        vps_wkt_next(parser);
    }
}

static int vps_wkt_polygon_body(VpsWktParser *parser, uint32_t dimensions)
{
    if (parser->token != VPS_WKT_LEFT) return 0;
    vps_wkt_next(parser);
    for (;;) {
        if (!vps_wkt_coordinate_list(parser, dimensions, 4U)) return 0;
        if (parser->token == VPS_WKT_RIGHT) {
            vps_wkt_next(parser);
            return 1;
        }
        if (parser->token != VPS_WKT_COMMA) return 0;
        vps_wkt_next(parser);
    }
}

static int vps_wkt_point_body(VpsWktParser *parser, uint32_t dimensions)
{
    uint32_t before = parser->result.point_count;
    return vps_wkt_coordinate_list(parser, dimensions, 1U) &&
           parser->result.point_count - before == 1U;
}

static VpsSpatialGeometryType vps_wkt_type(const VpsWktParser *parser)
{
    if (vps_wkt_word(parser, "POINT")) return VPS_SPATIAL_TYPE_POINT;
    if (vps_wkt_word(parser, "LINESTRING")) return VPS_SPATIAL_TYPE_LINESTRING;
    if (vps_wkt_word(parser, "POLYGON")) return VPS_SPATIAL_TYPE_POLYGON;
    if (vps_wkt_word(parser, "MULTIPOINT")) return VPS_SPATIAL_TYPE_MULTIPOINT;
    if (vps_wkt_word(parser, "MULTILINESTRING"))
        return VPS_SPATIAL_TYPE_MULTILINESTRING;
    if (vps_wkt_word(parser, "MULTIPOLYGON"))
        return VPS_SPATIAL_TYPE_MULTIPOLYGON;
    if (vps_wkt_word(parser, "GEOMETRYCOLLECTION"))
        return VPS_SPATIAL_TYPE_GEOMETRYCOLLECTION;
    return VPS_SPATIAL_TYPE_ANY;
}

static int vps_wkt_geometry(VpsWktParser *parser, uint32_t depth,
                            VpsSpatialGeometryType expected,
                            uint32_t inherited_dimensions)
{
    VpsSpatialGeometryType type;
    uint32_t dimensions = inherited_dimensions == 0U ? 2U
                                                     : inherited_dimensions;
    if (depth > parser->limits.max_depth || parser->token != VPS_WKT_WORD)
        return 0;
    type = vps_wkt_type(parser);
    if (type == VPS_SPATIAL_TYPE_ANY ||
        (expected != VPS_SPATIAL_TYPE_ANY && expected != type)) return 0;
    if (depth == 1U) parser->result.type = type;
    vps_wkt_next(parser);
    if (vps_wkt_word(parser, "Z")) { dimensions = 3U; vps_wkt_next(parser); }
    else if (vps_wkt_word(parser, "M")) {
        dimensions = 3U; vps_wkt_next(parser);
    } else if (vps_wkt_word(parser, "ZM")) {
        dimensions = 4U; vps_wkt_next(parser);
    }
    if (depth == 1U) parser->result.dimensions = dimensions;
    if (vps_wkt_word(parser, "EMPTY")) {
        parser->result.empty = 1;
        vps_wkt_next(parser);
        return 1;
    }
    if (type == VPS_SPATIAL_TYPE_POINT)
        return vps_wkt_point_body(parser, dimensions);
    if (type == VPS_SPATIAL_TYPE_LINESTRING)
        return vps_wkt_coordinate_list(parser, dimensions, 2U);
    if (type == VPS_SPATIAL_TYPE_POLYGON)
        return vps_wkt_polygon_body(parser, dimensions);
    if (type == VPS_SPATIAL_TYPE_MULTIPOINT) {
        if (parser->token != VPS_WKT_LEFT) return 0;
        vps_wkt_next(parser);
        for (;;) {
            if (parser->token == VPS_WKT_LEFT) {
                if (!vps_wkt_point_body(parser, dimensions)) return 0;
            } else if (!vps_wkt_coordinate(parser, dimensions)) return 0;
            if (parser->token == VPS_WKT_RIGHT) {
                vps_wkt_next(parser); return 1;
            }
            if (parser->token != VPS_WKT_COMMA) return 0;
            vps_wkt_next(parser);
        }
    }
    if (type == VPS_SPATIAL_TYPE_MULTILINESTRING ||
        type == VPS_SPATIAL_TYPE_MULTIPOLYGON) {
        if (parser->token != VPS_WKT_LEFT) return 0;
        vps_wkt_next(parser);
        for (;;) {
            if (type == VPS_SPATIAL_TYPE_MULTILINESTRING) {
                if (!vps_wkt_coordinate_list(parser, dimensions, 2U)) return 0;
            } else if (!vps_wkt_polygon_body(parser, dimensions)) return 0;
            if (parser->token == VPS_WKT_RIGHT) {
                vps_wkt_next(parser); return 1;
            }
            if (parser->token != VPS_WKT_COMMA) return 0;
            vps_wkt_next(parser);
        }
    }
    if (parser->token != VPS_WKT_LEFT) return 0;
    vps_wkt_next(parser);
    for (;;) {
        if (!vps_wkt_geometry(parser, depth + 1U, VPS_SPATIAL_TYPE_ANY,
                              dimensions)) return 0;
        if (parser->token == VPS_WKT_RIGHT) {
            vps_wkt_next(parser); return 1;
        }
        if (parser->token != VPS_WKT_COMMA) return 0;
        vps_wkt_next(parser);
    }
}

static VpsSpatialLimits vps_spatial_limits(const VpsSpatialLimits *limits)
{
    VpsSpatialLimits effective;
    effective.max_bytes = limits != NULL && limits->max_bytes != 0U
                              ? limits->max_bytes
                              : VPS_SPATIAL_DEFAULT_MAX_BYTES;
    effective.max_points = limits != NULL && limits->max_points != 0U
                               ? limits->max_points
                               : VPS_SPATIAL_DEFAULT_MAX_POINTS;
    effective.max_depth = limits != NULL && limits->max_depth != 0U
                              ? limits->max_depth
                              : VPS_SPATIAL_DEFAULT_MAX_DEPTH;
    return effective;
}

VpsSpatialResult vps_spatial_validate_text(
    const char *text, size_t length, VpsSpatialFormat format,
    VpsSpatialGeometryType expected_type, uint32_t expected_srid,
    const VpsSpatialLimits *limits, VpsSpatialValidation *validation)
{
    VpsWktParser parser;
    uint64_t srid = 0U;
    if (text == NULL || validation == NULL ||
        (format != VPS_SPATIAL_FORMAT_WKT &&
         format != VPS_SPATIAL_FORMAT_EWKT) ||
        expected_type > VPS_SPATIAL_TYPE_GEOMETRYCOLLECTION)
        return VPS_SPATIAL_INVALID_ARGUMENT;
    (void)memset(&parser, 0, sizeof(parser));
    parser.text = text;
    parser.length = length;
    parser.limits = vps_spatial_limits(limits);
    if (length == 0U || length > parser.limits.max_bytes ||
        parser.limits.max_points == 0U || parser.limits.max_depth == 0U)
        return length > parser.limits.max_bytes ? VPS_SPATIAL_LIMIT_EXCEEDED
                                               : VPS_SPATIAL_MALFORMED;
    vps_wkt_next(&parser);
    if (format == VPS_SPATIAL_FORMAT_EWKT) {
        if (!vps_wkt_word(&parser, "SRID")) goto malformed;
        vps_wkt_next(&parser);
        if (parser.token != VPS_WKT_EQUAL) goto malformed;
        vps_wkt_next(&parser);
        if (parser.token != VPS_WKT_NUMBER ||
            parser.text[parser.token_offset] == '-' ||
            parser.token_length > 10U) goto malformed;
        while (parser.token_length != 0U) {
            unsigned char digit = (unsigned char)parser.text[parser.token_offset++];
            parser.token_length -= 1U;
            if (digit < '0' || digit > '9') goto malformed;
            srid = srid * UINT64_C(10) + (uint64_t)(digit - '0');
            if (srid > UINT32_MAX) goto malformed;
        }
        parser.result.srid = (uint32_t)srid;
        parser.result.has_srid = 1;
        vps_wkt_next(&parser);
        if (parser.token != VPS_WKT_SEMICOLON) goto malformed;
        vps_wkt_next(&parser);
        if (expected_srid != 0U && expected_srid != parser.result.srid) {
            *validation = parser.result;
            return VPS_SPATIAL_SRID_MISMATCH;
        }
    } else if (expected_srid != 0U) {
        parser.result.srid = expected_srid;
        parser.result.has_srid = 1;
    }
    if (!vps_wkt_geometry(&parser, 1U, expected_type, 0U) ||
        parser.token != VPS_WKT_END) goto malformed;
    *validation = parser.result;
    return VPS_SPATIAL_OK;
malformed:
    parser.result.error_offset = parser.token_offset;
    *validation = parser.result;
    return parser.result.point_count >= parser.limits.max_points
               ? VPS_SPATIAL_LIMIT_EXCEEDED : VPS_SPATIAL_MALFORMED;
}

typedef struct VpsWkbParser {
    const unsigned char *bytes;
    size_t length;
    size_t offset;
    VpsSpatialLimits limits;
    VpsSpatialValidation result;
    int limit_failure;
} VpsWkbParser;

static int vps_wkb_uint32(VpsWkbParser *parser, int little, uint32_t *value)
{
    const unsigned char *data;
    if (parser->offset > parser->length ||
        parser->length - parser->offset < 4U) return 0;
    data = parser->bytes + parser->offset;
    parser->offset += 4U;
    *value = little
                 ? (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
                       ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U)
                 : (uint32_t)data[3] | ((uint32_t)data[2] << 8U) |
                       ((uint32_t)data[1] << 16U) | ((uint32_t)data[0] << 24U);
    return 1;
}

static int vps_wkb_double(VpsWkbParser *parser, int little,
                          double *value)
{
    unsigned char native[8];
    size_t index;
    const uint16_t endian_test = UINT16_C(1);
    int host_little = *(const unsigned char *)&endian_test == 1U;
    if (parser->offset > parser->length ||
        parser->length - parser->offset < 8U) return 0;
    for (index = 0U; index < 8U; ++index)
        native[host_little == little ? index : 7U - index] =
            parser->bytes[parser->offset + index];
    parser->offset += 8U;
    (void)memcpy(value, native, sizeof(*value));
    return 1;
}

static int vps_wkb_point(VpsWkbParser *parser, int little,
                         uint32_t dimensions, int *empty)
{
    uint32_t dimension;
    int nan_count = 0;
    *empty = 0;
    for (dimension = 0U; dimension < dimensions; ++dimension) {
        double coordinate;
        if (!vps_wkb_double(parser, little, &coordinate)) return 0;
        if (isnan(coordinate)) nan_count += 1;
        else if (!isfinite(coordinate)) return 0;
    }
    if (nan_count != 0 && nan_count != (int)dimensions) return 0;
    *empty = nan_count == (int)dimensions;
    if (!*empty) {
        if (parser->result.point_count == parser->limits.max_points) {
            parser->limit_failure = 1;
            return 0;
        }
        parser->result.point_count += 1U;
    }
    return 1;
}

static int vps_wkb_geometry(VpsWkbParser *parser, uint32_t depth,
                            VpsSpatialGeometryType expected,
                            uint32_t inherited_dimensions)
{
    int little;
    uint32_t raw_type;
    uint32_t type_number;
    uint32_t dimensions = 2U;
    uint32_t srid = 0U;
    int has_srid = 0;
    uint32_t count;
    uint32_t index;
    VpsSpatialGeometryType type;
    if (depth > parser->limits.max_depth || parser->offset >= parser->length) {
        parser->limit_failure = depth > parser->limits.max_depth;
        return 0;
    }
    little = parser->bytes[parser->offset++];
    if (little != 0 && little != 1) return 0;
    if (!vps_wkb_uint32(parser, little, &raw_type)) return 0;
    type_number = raw_type & UINT32_C(0x0fffffff);
    if ((raw_type & UINT32_C(0x80000000)) != 0U) dimensions += 1U;
    if ((raw_type & UINT32_C(0x40000000)) != 0U) dimensions += 1U;
    has_srid = (raw_type & UINT32_C(0x20000000)) != 0U;
    if ((raw_type & UINT32_C(0xe0000000)) == 0U && type_number >= 1000U) {
        uint32_t family = type_number / 1000U;
        if (family < 1U || family > 3U) return 0;
        dimensions = family == 3U ? 4U : 3U;
        type_number %= 1000U;
    }
    if (type_number < 1U || type_number > 7U) return 0;
    type = (VpsSpatialGeometryType)type_number;
    if (expected != VPS_SPATIAL_TYPE_ANY && expected != type) return 0;
    if (inherited_dimensions != 0U && inherited_dimensions != dimensions)
        return 0;
    if (has_srid && !vps_wkb_uint32(parser, little, &srid)) return 0;
    if (depth == 1U) {
        parser->result.type = type;
        parser->result.dimensions = dimensions;
        parser->result.has_srid = has_srid;
        parser->result.srid = srid;
    }
    if (type == VPS_SPATIAL_TYPE_POINT) {
        int empty;
        if (!vps_wkb_point(parser, little, dimensions, &empty)) return 0;
        if (depth == 1U) parser->result.empty = empty;
        return 1;
    }
    if (!vps_wkb_uint32(parser, little, &count)) return 0;
    if (count > parser->limits.max_points - parser->result.point_count &&
        (type == VPS_SPATIAL_TYPE_LINESTRING ||
         type == VPS_SPATIAL_TYPE_MULTIPOINT)) {
        parser->limit_failure = 1;
        return 0;
    }
    if (count == 0U && depth == 1U) parser->result.empty = 1;
    if (type == VPS_SPATIAL_TYPE_LINESTRING) {
        if (count != 0U && count < 2U) return 0;
        for (index = 0U; index < count; ++index) {
            int empty;
            if (!vps_wkb_point(parser, little, dimensions, &empty) || empty)
                return 0;
        }
        return 1;
    }
    if (type == VPS_SPATIAL_TYPE_POLYGON) {
        for (index = 0U; index < count; ++index) {
            uint32_t points;
            uint32_t point;
            if (!vps_wkb_uint32(parser, little, &points) ||
                (points != 0U && points < 4U) ||
                points > parser->limits.max_points - parser->result.point_count)
                return 0;
            for (point = 0U; point < points; ++point) {
                int empty;
                if (!vps_wkb_point(parser, little, dimensions, &empty) || empty)
                    return 0;
            }
        }
        return 1;
    }
    for (index = 0U; index < count; ++index) {
        VpsSpatialGeometryType child =
            type == VPS_SPATIAL_TYPE_MULTIPOINT ? VPS_SPATIAL_TYPE_POINT
            : type == VPS_SPATIAL_TYPE_MULTILINESTRING
                ? VPS_SPATIAL_TYPE_LINESTRING
            : type == VPS_SPATIAL_TYPE_MULTIPOLYGON
                ? VPS_SPATIAL_TYPE_POLYGON : VPS_SPATIAL_TYPE_ANY;
        if (!vps_wkb_geometry(parser, depth + 1U, child, dimensions)) return 0;
    }
    return 1;
}

VpsSpatialResult vps_spatial_validate_binary(
    const void *bytes, size_t length, VpsSpatialFormat format,
    VpsSpatialGeometryType expected_type, uint32_t expected_srid,
    const VpsSpatialLimits *limits, VpsSpatialValidation *validation)
{
    VpsWkbParser parser;
    if (bytes == NULL || validation == NULL ||
        (format != VPS_SPATIAL_FORMAT_WKB &&
         format != VPS_SPATIAL_FORMAT_EWKB) ||
        expected_type > VPS_SPATIAL_TYPE_GEOMETRYCOLLECTION)
        return VPS_SPATIAL_INVALID_ARGUMENT;
    (void)memset(&parser, 0, sizeof(parser));
    parser.bytes = (const unsigned char *)bytes;
    parser.length = length;
    parser.limits = vps_spatial_limits(limits);
    if (length == 0U || length > parser.limits.max_bytes)
        return length > parser.limits.max_bytes ? VPS_SPATIAL_LIMIT_EXCEEDED
                                               : VPS_SPATIAL_MALFORMED;
    if (!vps_wkb_geometry(&parser, 1U, expected_type, 0U) ||
        parser.offset != parser.length) {
        parser.result.error_offset = parser.offset;
        *validation = parser.result;
        return parser.limit_failure ? VPS_SPATIAL_LIMIT_EXCEEDED
                                    : VPS_SPATIAL_MALFORMED;
    }
    if (format == VPS_SPATIAL_FORMAT_EWKB && !parser.result.has_srid)
        return VPS_SPATIAL_MALFORMED;
    if (expected_srid != 0U) {
        if (parser.result.has_srid && parser.result.srid != expected_srid) {
            *validation = parser.result;
            return VPS_SPATIAL_SRID_MISMATCH;
        }
        if (!parser.result.has_srid) {
            parser.result.has_srid = 1;
            parser.result.srid = expected_srid;
        }
    }
    *validation = parser.result;
    return VPS_SPATIAL_OK;
}

void vps_spatial_capabilities_reset(VpsSpatialCapabilities *capabilities)
{
    if (capabilities == NULL) return;
    vps_buffer_reset(&capabilities->schema);
    vps_buffer_reset(&capabilities->version);
    capabilities->namespace_oid = 0U;
    capabilities->geometry_oid = 0U;
    capabilities->geography_oid = 0U;
    capabilities->flags = 0U;
    capabilities->present = 0;
    capabilities->initialized = 0;
}

const char *vps_spatial_result_name(VpsSpatialResult result)
{
    switch (result) {
        case VPS_SPATIAL_OK: return "ok";
        case VPS_SPATIAL_INVALID_ARGUMENT: return "invalid_argument";
        case VPS_SPATIAL_INVALID_RESULT: return "invalid_result";
        case VPS_SPATIAL_NOT_AVAILABLE: return "not_available";
        case VPS_SPATIAL_UNSUPPORTED: return "unsupported";
        case VPS_SPATIAL_MALFORMED: return "malformed";
        case VPS_SPATIAL_LIMIT_EXCEEDED: return "limit_exceeded";
        case VPS_SPATIAL_SRID_MISMATCH: return "srid_mismatch";
        case VPS_SPATIAL_OUT_OF_MEMORY: return "out_of_memory";
        default: return "unknown";
    }
}

const char *vps_spatial_format_name(VpsSpatialFormat format)
{
    switch (format) {
        case VPS_SPATIAL_FORMAT_WKT: return "wkt";
        case VPS_SPATIAL_FORMAT_WKB: return "wkb";
        case VPS_SPATIAL_FORMAT_EWKT: return "ewkt";
        case VPS_SPATIAL_FORMAT_EWKB: return "ewkb";
        case VPS_SPATIAL_FORMAT_NONE: return "none";
        case VPS_SPATIAL_FORMAT_SPATIALITE: return "spatialite";
        default: return "unknown";
    }
}

VpsSpatialResult vps_spatial_typmod_decode(
    int32_t type_modifier, VpsSpatialGeometryType *type,
    uint32_t *dimensions, uint32_t *srid)
{
    uint32_t packed;
    uint32_t raw_type;
    if (type == NULL || dimensions == NULL || srid == NULL)
        return VPS_SPATIAL_INVALID_ARGUMENT;
    *type = VPS_SPATIAL_TYPE_ANY;
    *dimensions = 0U;
    *srid = 0U;
    if (type_modifier < 0) return VPS_SPATIAL_OK;
    packed = (uint32_t)type_modifier;
    raw_type = (packed >> 2U) & UINT32_C(0x3f);
    if (raw_type > (uint32_t)VPS_SPATIAL_TYPE_GEOMETRYCOLLECTION)
        return VPS_SPATIAL_UNSUPPORTED;
    *type = (VpsSpatialGeometryType)raw_type;
    *dimensions = 2U + ((packed & UINT32_C(0x2)) != 0U ? 1U : 0U) +
                  ((packed & UINT32_C(0x1)) != 0U ? 1U : 0U);
    *srid = (packed >> 8U) & UINT32_C(0x1fffff);
    return VPS_SPATIAL_OK;
}
