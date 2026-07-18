[← Previous: Spatial](spatial.md) · [Back to README](../README.md) · [Next: Static analysis →](static-analysis.md)

# Metadata functions, cache, and integrity

VirtualPostgreSQL exposes six `DIRECTONLY` eponymous table-valued functions:

- `virtualpostgresql_relations(connection, schema)`
- `virtualpostgresql_table_info(connection, schema, relation)`
- `virtualpostgresql_index_list(connection, schema, relation)`
- `virtualpostgresql_index_info(connection, schema, relation, index_name)`
- `virtualpostgresql_type_info(connection, type_schema, type_name)`
- `virtualpostgresql_extensions(connection)`

`connection` is PostgreSQL conninfo supplied at runtime. It is a hidden
argument and is never included in result rows, logs, shadow tables, or error
messages. Catalog statements use bound parameters and explicitly qualified
`pg_catalog` objects. Result row counts, field counts, individual cells, and
total copied bytes are bounded.

The `table_info` and index functions begin with SQLite-compatible discovery
columns and then expose PostgreSQL OIDs, typmods, relation/index state,
collation/opclass, identity/generated fields, and trusted PostGIS metadata.
Relation statistics are estimates; `statistics_available` tells consumers
whether PostgreSQL supplied an estimate.

## Persistent metadata

Each persistent VirtualPostgreSQL table owns exactly one row in each shadow
table:

- `<vtab>_vps_schema` records format, source/layout fingerprints, relation and
  configuration generation, capture time, codec version, and extension
  version.
- `<vtab>_vps_metadata` stores one deterministic, checksummed portable blob
  and its last-validation time.

The blob is capped at 1 MiB and uses explicit little-endian fields and lengths;
it is not a dump of a C structure. It contains only the declaration layout,
type/origin/key/spatial policy, fingerprints, and non-secret generations. It
never contains conninfo, credentials, SQL text, bound values, or result rows.

`metadata_mode=live` requires live catalog validation while connecting.
`metadata_mode=cached` may use the shadow declaration only after a connection
or timeout failure and only when the canonical connection identity still
matches. Authentication, TLS, configuration, SQL, and schema errors never
fall back. Before the first row scan, cached declarations must pass live
validation.

`schema_policy=strict` returns `SQLITE_SCHEMA` for relevant drift.
`schema_policy=refresh` refreshes only a compatible snapshot with the same
visible names/types, relation origins, keys, spatial codecs/SRIDs, and relation
identity; narrowing or drop/recreate remains an error.

SQLite `PRAGMA integrity_check` validates shadow row counts, versions, bounds,
checksum/decode, unique field names, key references, spatial metadata, and the
schema-row fingerprints without contacting PostgreSQL. A consistency failure
is reported as integrity text through SQLite's `xIntegrity` contract.

## See Also

- [Table metadata](table-metadata.md) — live catalog snapshot.
- [Security](security.md) — auth/TLS errors are never masked.
- [Troubleshooting](troubleshooting.md) — schema drift recovery.
