[← Previous: Transactions](transactions-savepoints.md) · [Back to README](../README.md) · [Next: Metadata and cache →](metadata-functions-cache.md)

# PostGIS and spatial values

PostGIS support is optional and is discovered only through PostgreSQL catalogs.
The extension schema may be non-default; spatial function calls are always
schema-qualified using the trusted discovery result.

| Mode | SQLite representation | SRID |
|---|---|---|
| `geometry=wkt` | TEXT WKT | typmod or explicit `srid=` |
| `geometry=wkb` | BLOB WKB | typmod or explicit `srid=` |
| `geometry=ewkt` | TEXT EWKT | embedded and typmod SRIDs must agree |
| `geometry=ewkb` | BLOB EWKB | embedded and typmod SRIDs must agree |

Both `geometry` and `geography` are supported, and EMPTY remains distinct from
NULL. Structural validators check endianness, type, dimensions, SRID, counts,
nesting, finite coordinates, and exact payload length before sending data to
the server. The extension does not swap coordinate axes, call `ST_Transform`,
or substitute an SRID.

SpatiaLite output is currently unavailable. Writes are allowed
only for a writable table source with compatible catalog capabilities; query
sources, views, materialized views, and foreign tables remain read-only.

Examples are available in [examples/spatial.sql](../examples/spatial.sql).

## See Also

- [Type mapping](type-mapping.md) — scalar codec policy.
- [Query sources](query-sources.md) — result overrides.
- [Security](security.md) — payload redaction.
