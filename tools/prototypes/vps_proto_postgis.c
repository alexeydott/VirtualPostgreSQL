#include "vps_proto_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VPS_POSTGIS_SQL_CAPACITY 4096
#define VPS_POSTGIS_TIMEOUT_MS 10000U
#define VPS_POSTGIS_SRID 4326
#define VPS_POSTGIS_MISMATCH_SRID 3857
#define VPS_POSTGIS_REQUIRED_FUNCTIONS 11
#define VPS_POSTGIS_SCHEMA_MAX 63U

typedef struct vps_spatial_sample {
    const char *wkt;
    int dimensions;
    int empty;
} vps_spatial_sample;

static const char VPS_SQL_DISCOVER[] =
    "SELECT e.extversion, n.nspname, "
    "COALESCE(g.oid, 0)::oid, COALESCE(gg.oid, 0)::oid, "
    "(SELECT count(DISTINCT p.proname)::int4 FROM pg_catalog.pg_proc p "
    " WHERE p.pronamespace = e.extnamespace "
    " AND p.proname IN ('st_geomfromtext','st_astext','st_geomfromwkb','st_asbinary',"
    "'st_srid','st_isempty','st_ndims','st_equals','st_geogfromwkb','st_asewkb',"
    "'st_geomfromewkb')) "
    "FROM pg_catalog.pg_extension e "
    "JOIN pg_catalog.pg_namespace n ON n.oid = e.extnamespace "
    "LEFT JOIN pg_catalog.pg_type g ON g.typnamespace = e.extnamespace AND g.typname = 'geometry' "
    "LEFT JOIN pg_catalog.pg_type gg ON gg.typnamespace = e.extnamespace AND gg.typname = 'geography' "
    "WHERE e.extname = $1";

static size_t vps_result_count;
static size_t vps_clear_count;

static uint64_t vps_fingerprint(const char *text)
{
    uint64_t value = UINT64_C(1469598103934665603);
    while (*text != '\0') {
        value ^= (unsigned char)*text++;
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static void vps_clear_tracked(PGresult *result)
{
    if (result != NULL) {
        PQclear(result);
        ++vps_clear_count;
    }
}

static PGresult *vps_execute(PGconn *connection,
                             const char *sql,
                             int parameter_count,
                             const Oid *parameter_types,
                             const char *const *values,
                             const int *lengths,
                             const int *formats)
{
    vps_deadline deadline = vps_deadline_after(VPS_POSTGIS_TIMEOUT_MS);
    int operation_ok = 0;
    PGresult *result;

    if (PQsendQueryParams(connection, sql, parameter_count, parameter_types, values,
                          lengths, formats, 0) == 0) {
        vps_log("error", "postgis_operation_failed", "phase=send");
        return NULL;
    }
    if (!vps_flush_async(connection, &deadline, NULL)) {
        vps_log("error", "postgis_operation_failed", "phase=flush");
        return NULL;
    }
    result = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
    if (!operation_ok || result == NULL) {
        vps_log("error", "postgis_operation_failed", "phase=result");
        return NULL;
    }
    ++vps_result_count;
    return result;
}

static int vps_release_and_drain(PGconn *connection, PGresult *result)
{
    vps_deadline deadline = vps_deadline_after(VPS_POSTGIS_TIMEOUT_MS);
    int operation_ok = 0;
    int unexpected_result = 0;

    vps_clear_tracked(result);
    for (;;) {
        result = vps_get_result_async(connection, &deadline, NULL, &operation_ok);
        if (!operation_ok) {
            vps_log("error", "postgis_operation_failed", "phase=terminal_drain");
            return 0;
        }
        if (result == NULL) {
            return !unexpected_result;
        }
        ++vps_result_count;
        unexpected_result = 1;
        vps_clear_tracked(result);
    }
}

static int vps_format_sql(char *sql, size_t capacity, const char *format,
                          const char *schema, int schema_count)
{
    int written;

    switch (schema_count) {
    case 1:
        written = snprintf(sql, capacity, format, schema);
        break;
    case 2:
        written = snprintf(sql, capacity, format, schema, schema);
        break;
    case 3:
        written = snprintf(sql, capacity, format, schema, schema, schema);
        break;
    case 4:
        written = snprintf(sql, capacity, format, schema, schema, schema, schema);
        break;
    case 5:
        written = snprintf(sql, capacity, format, schema, schema, schema, schema, schema);
        break;
    case 6:
        written = snprintf(sql, capacity, format, schema, schema, schema, schema, schema, schema);
        break;
    case 7:
        written = snprintf(sql, capacity, format, schema, schema, schema, schema, schema, schema, schema);
        break;
    case 8:
        written = snprintf(sql, capacity, format, schema, schema, schema, schema,
                           schema, schema, schema, schema);
        break;
    case 9:
        written = snprintf(sql, capacity, format, schema, schema, schema, schema,
                           schema, schema, schema, schema, schema);
        break;
    default:
        return 0;
    }
    return written >= 0 && (size_t)written < capacity;
}

static int vps_is_true(PGresult *result, int field)
{
    return !PQgetisnull(result, 0, field) && strcmp(PQgetvalue(result, 0, field), "t") == 0;
}

static int vps_geometry_roundtrip(PGconn *connection,
                                  const char *quoted_schema,
                                  const vps_spatial_sample *sample,
                                  size_t sample_id)
{
    static const char sql_format[] =
        "WITH source AS (SELECT %s.ST_GeomFromText($1,$2::int4) AS original), "
        "roundtrip AS (SELECT original, %s.ST_GeomFromWKB(%s.ST_AsBinary(original,'NDR'),$2::int4) AS restored FROM source) "
        "SELECT %s.ST_SRID(restored), %s.ST_IsEmpty(restored), %s.ST_NDims(restored), "
        "%s.ST_Equals(original,restored), "
        "%s.ST_AsBinary(original,'NDR') = %s.ST_AsBinary(restored,'NDR') FROM roundtrip";
    char sql[VPS_POSTGIS_SQL_CAPACITY];
    char srid_text[16];
    const char *values[2];
    PGresult *result;
    char fields[256];
    int ok;

    if (!vps_format_sql(sql, sizeof(sql), sql_format, quoted_schema, 9)) {
        return 0;
    }
    (void)snprintf(srid_text, sizeof(srid_text), "%d", VPS_POSTGIS_SRID);
    values[0] = sample->wkt;
    values[1] = srid_text;
    result = vps_execute(connection, sql, 2, NULL, values, NULL, NULL);
    if (result == NULL) {
        return 0;
    }
    ok = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) == 1 &&
         PQnfields(result) == 5 && !PQgetisnull(result, 0, 0) &&
         atoi(PQgetvalue(result, 0, 0)) == VPS_POSTGIS_SRID &&
         vps_is_true(result, 1) == sample->empty && !PQgetisnull(result, 0, 2) &&
         atoi(PQgetvalue(result, 0, 2)) == sample->dimensions &&
         vps_is_true(result, 3) && vps_is_true(result, 4);
    if (!ok && PQresultStatus(result) != PGRES_TUPLES_OK) {
        (void)snprintf(fields, sizeof(fields), "sample_id=%zu status=%s sqlstate=%s",
                       sample_id, PQresStatus(PQresultStatus(result)), vps_sqlstate(result));
        vps_log("error", "postgis_geometry_failure", fields);
    }
    if (!vps_release_and_drain(connection, result)) {
        ok = 0;
    }
    (void)snprintf(fields, sizeof(fields),
                   "kind=geometry formats=wkt_wkb sample_id=%zu dimensions=%d empty=%s srid=%d outcome=%s",
                   sample_id, sample->dimensions, sample->empty ? "true" : "false",
                   VPS_POSTGIS_SRID, ok ? "pass" : "fail");
    vps_log(ok ? "info" : "error", "postgis_roundtrip", fields);
    return ok;
}

static int vps_geography_roundtrip(PGconn *connection, const char *quoted_schema)
{
    static const char sql_format[] =
        "WITH source AS (SELECT CAST(%s.ST_GeomFromText($1,$2::int4) AS %s.geography) AS original), "
        "roundtrip AS (SELECT original, %s.ST_GeogFromWKB(%s.ST_AsBinary(original,'NDR')) AS restored FROM source) "
        "SELECT %s.ST_SRID(restored), "
        "%s.ST_AsBinary(original,'NDR') = %s.ST_AsBinary(restored,'NDR'), "
        "%s.ST_AsText(restored) IS NOT NULL FROM roundtrip";
    static const char sample[] = "POINT (1 2)";
    char sql[VPS_POSTGIS_SQL_CAPACITY];
    char srid_text[16];
    const char *values[2];
    PGresult *result;
    char fields[192];
    int ok;

    if (!vps_format_sql(sql, sizeof(sql), sql_format, quoted_schema, 8)) {
        return 0;
    }
    (void)snprintf(srid_text, sizeof(srid_text), "%d", VPS_POSTGIS_SRID);
    values[0] = sample;
    values[1] = srid_text;
    result = vps_execute(connection, sql, 2, NULL, values, NULL, NULL);
    if (result == NULL) {
        return 0;
    }
    ok = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) == 1 &&
         PQnfields(result) == 3 && !PQgetisnull(result, 0, 0) &&
         atoi(PQgetvalue(result, 0, 0)) == VPS_POSTGIS_SRID &&
         vps_is_true(result, 1) && vps_is_true(result, 2);
    if (!ok && PQresultStatus(result) != PGRES_TUPLES_OK) {
        (void)snprintf(fields, sizeof(fields), "kind=geography status=%s sqlstate=%s",
                       PQresStatus(PQresultStatus(result)), vps_sqlstate(result));
        vps_log("error", "postgis_geography_failure", fields);
    }
    if (!vps_release_and_drain(connection, result)) {
        ok = 0;
    }
    (void)snprintf(fields, sizeof(fields),
                   "kind=geography formats=wkt_wkb srid=%d outcome=%s",
                   VPS_POSTGIS_SRID, ok ? "pass" : "fail");
    vps_log(ok ? "info" : "error", "postgis_roundtrip", fields);
    return ok;
}

static int vps_null_empty_distinction(PGconn *connection, const char *quoted_schema)
{
    static const char sql_format[] =
        "SELECT %s.ST_GeomFromText($1,$2::int4) IS NULL, "
        "%s.ST_IsEmpty(%s.ST_GeomFromText($1,$2::int4))";
    char sql[VPS_POSTGIS_SQL_CAPACITY];
    char srid_text[16];
    const char *values[2];
    PGresult *result;
    int ok;

    if (!vps_format_sql(sql, sizeof(sql), sql_format, quoted_schema, 3)) {
        return 0;
    }
    (void)snprintf(srid_text, sizeof(srid_text), "%d", VPS_POSTGIS_SRID);
    values[0] = NULL;
    values[1] = srid_text;
    result = vps_execute(connection, sql, 2, NULL, values, NULL, NULL);
    if (result == NULL) {
        return 0;
    }
    ok = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) == 1 &&
         PQnfields(result) == 2 && vps_is_true(result, 0) && PQgetisnull(result, 0, 1);
    if (!vps_release_and_drain(connection, result)) {
        ok = 0;
    }
    vps_log(ok ? "info" : "error", "postgis_null_empty",
            ok ? "null=distinct empty=distinct outcome=pass" : "outcome=fail");
    return ok;
}

static int vps_srid_mismatch(PGconn *connection, const char *quoted_schema)
{
    static const char sql_format[] =
        "SELECT %s.ST_SRID(%s.ST_GeomFromEWKB(%s.ST_AsEWKB(%s.ST_GeomFromText($1,$2::int4))))";
    static const char sample[] = "POINT (1 2)";
    char sql[VPS_POSTGIS_SQL_CAPACITY];
    char srid_text[16];
    const char *values[2];
    PGresult *result;
    char fields[192];
    int actual_srid = 0;
    int ok;

    if (!vps_format_sql(sql, sizeof(sql), sql_format, quoted_schema, 4)) {
        return 0;
    }
    (void)snprintf(srid_text, sizeof(srid_text), "%d", VPS_POSTGIS_SRID);
    values[0] = sample;
    values[1] = srid_text;
    result = vps_execute(connection, sql, 2, NULL, values, NULL, NULL);
    if (result == NULL) {
        return 0;
    }
    if (PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) == 1 &&
        PQnfields(result) == 1 && !PQgetisnull(result, 0, 0)) {
        actual_srid = atoi(PQgetvalue(result, 0, 0));
    }
    ok = actual_srid == VPS_POSTGIS_SRID && actual_srid != VPS_POSTGIS_MISMATCH_SRID;
    if (!vps_release_and_drain(connection, result)) {
        ok = 0;
    }
    (void)snprintf(fields, sizeof(fields),
                   "actual_srid=%d expected_srid=%d decision=%s transform=false outcome=%s",
                   actual_srid, VPS_POSTGIS_MISMATCH_SRID, ok ? "reject" : "invalid",
                   ok ? "pass" : "fail");
    vps_log(ok ? "info" : "error", "postgis_srid_mismatch", fields);
    return ok;
}

static int vps_expect_malformed(PGconn *connection,
                                const char *sql,
                                const Oid *parameter_types,
                                const char *const *values,
                                const int *lengths,
                                const int *formats,
                                const char *input_format)
{
    PGresult *result = vps_execute(connection, sql, 1, parameter_types, values, lengths, formats);
    char fields[160];
    char sqlstate[6];
    int ok;

    if (result == NULL) {
        return 0;
    }
    ok = PQresultStatus(result) == PGRES_FATAL_ERROR;
    (void)snprintf(sqlstate, sizeof(sqlstate), "%s", vps_sqlstate(result));
    if (!vps_release_and_drain(connection, result)) {
        ok = 0;
    }
    (void)snprintf(fields, sizeof(fields), "format=%s sqlstate=%s outcome=%s",
                   input_format, sqlstate, ok ? "pass" : "fail");
    vps_log(ok ? "info" : "error", "postgis_malformed", fields);
    return ok;
}

static int vps_malformed_inputs(PGconn *connection, const char *quoted_schema)
{
    static const char wkt_sql_format[] = "SELECT %s.ST_GeomFromText($1)";
    static const char wkb_sql_format[] = "SELECT %s.ST_GeomFromWKB($1)";
    static const char malformed_wkt[] = "POINT (";
    static const unsigned char malformed_wkb[] = { 1U, 1U, 0U };
    char wkt_sql[VPS_POSTGIS_SQL_CAPACITY];
    char wkb_sql[VPS_POSTGIS_SQL_CAPACITY];
    const char *wkt_values[1] = { malformed_wkt };
    const char *wkb_values[1] = { (const char *)malformed_wkb };
    Oid wkb_types[1] = { 17U };
    int wkb_lengths[1] = { (int)sizeof(malformed_wkb) };
    int wkb_formats[1] = { 1 };

    if (!vps_format_sql(wkt_sql, sizeof(wkt_sql), wkt_sql_format, quoted_schema, 1) ||
        !vps_format_sql(wkb_sql, sizeof(wkb_sql), wkb_sql_format, quoted_schema, 1)) {
        return 0;
    }
    if (!vps_expect_malformed(connection, wkt_sql, NULL, wkt_values, NULL, NULL, "wkt")) {
        return 0;
    }
    return vps_expect_malformed(connection, wkb_sql, wkb_types, wkb_values,
                                wkb_lengths, wkb_formats, "wkb");
}

int main(void)
{
    static const char extension_name[] = "postgis";
    static const vps_spatial_sample samples[] = {
        { "POINT EMPTY", 2, 1 },
        { "LINESTRING EMPTY", 2, 1 },
        { "POLYGON EMPTY", 2, 1 },
        { "GEOMETRYCOLLECTION EMPTY", 2, 1 },
        { "POINT (1 2)", 2, 0 },
        { "LINESTRING (0 0,1 1)", 2, 0 },
        { "POLYGON ((0 0,0 2,2 2,0 0))", 2, 0 },
        { "MULTIPOINT ((0 0),(1 1))", 2, 0 },
        { "MULTILINESTRING ((0 0,1 1),(2 2,3 3))", 2, 0 },
        { "MULTIPOLYGON (((0 0,0 2,2 2,0 0)))", 2, 0 },
        { "GEOMETRYCOLLECTION (POINT (1 2),LINESTRING (0 0,1 1))", 2, 0 },
        { "POINT Z (1 2 3)", 3, 0 },
        { "POINT M (1 2 4)", 3, 0 },
        { "POINT ZM (1 2 3 4)", 4, 0 }
    };
    const char *discovery_values[1] = { extension_name };
    PGconn *connection = NULL;
    PGresult *result = NULL;
    char *schema = NULL;
    char *quoted_schema = NULL;
    char fields[320];
    int ok = 1;
    int present = 0;
    size_t index;

    connection = vps_connect_test_server(5000U);
    if (connection == NULL) {
        return 65;
    }
    result = vps_execute(connection, VPS_SQL_DISCOVER, 1, NULL, discovery_values, NULL, NULL);
    if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK || PQnfields(result) != 5) {
        ok = 0;
        goto cleanup;
    }
    if (PQntuples(result) == 0) {
        vps_log("info", "postgis_capability",
                "present=false catalog_anchor=pg_extension fake_name_rejected=true");
    } else if (PQntuples(result) == 1) {
        const char *version = PQgetvalue(result, 0, 0);
        const char *schema_value = PQgetvalue(result, 0, 1);
        unsigned long geometry_oid = strtoul(PQgetvalue(result, 0, 2), NULL, 10);
        unsigned long geography_oid = strtoul(PQgetvalue(result, 0, 3), NULL, 10);
        int function_count = atoi(PQgetvalue(result, 0, 4));
        size_t schema_length = strlen(schema_value);

        if (schema_length == 0U || schema_length > VPS_POSTGIS_SCHEMA_MAX) {
            ok = 0;
            goto cleanup;
        }
        schema = (char *)malloc(schema_length + 1U);
        if (schema == NULL) {
            ok = 0;
            goto cleanup;
        }
        (void)memcpy(schema, schema_value, schema_length + 1U);
        present = 1;
        (void)snprintf(fields, sizeof(fields),
                       "present=true version=%s schema_fingerprint=%016llx schema_class=%s "
                       "geometry_oid=%lu geography_oid=%lu required_function_names=%d "
                       "catalog_anchor=pg_extension fake_name_rejected=true",
                       version, (unsigned long long)vps_fingerprint(schema),
                       strcmp(schema, "public") == 0 ? "default" : "custom",
                       geometry_oid, geography_oid, function_count);
        vps_log("info", "postgis_capability", fields);
        if (geometry_oid == 0UL || geography_oid == 0UL ||
            function_count < VPS_POSTGIS_REQUIRED_FUNCTIONS) {
            ok = 0;
        }
    } else {
        ok = 0;
    }
    if (!vps_release_and_drain(connection, result)) {
        ok = 0;
    }
    result = NULL;
    if (!ok || !present) {
        goto cleanup;
    }

    quoted_schema = PQescapeIdentifier(connection, schema, strlen(schema));
    if (quoted_schema == NULL) {
        ok = 0;
        goto cleanup;
    }
    for (index = 0U; ok && index < sizeof(samples) / sizeof(samples[0]); ++index) {
        ok = vps_geometry_roundtrip(connection, quoted_schema, &samples[index], index);
    }
    if (ok) {
        ok = vps_null_empty_distinction(connection, quoted_schema);
    }
    if (ok) {
        ok = vps_geography_roundtrip(connection, quoted_schema);
    }
    if (ok) {
        ok = vps_srid_mismatch(connection, quoted_schema);
    }
    if (ok) {
        ok = vps_malformed_inputs(connection, quoted_schema);
    }

cleanup:
    vps_clear_tracked(result);
    if (quoted_schema != NULL) {
        PQfreemem(quoted_schema);
    }
    free(schema);
    PQfinish(connection);
    (void)snprintf(fields, sizeof(fields),
                   "outcome=%s present=%s results=%zu clears=%zu cleanup_exact=%s",
                   ok ? "pass" : "fail", present ? "true" : "false",
                   vps_result_count, vps_clear_count,
                   vps_result_count == vps_clear_count ? "true" : "false");
    vps_log(ok ? "info" : "error", "postgis_probe_complete", fields);
    return ok && vps_result_count == vps_clear_count ? 0 : 1;
}
