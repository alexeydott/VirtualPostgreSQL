[← Previous: Table metadata](table-metadata.md) · [Back to README](../README.md) · [Next: Query sources →](query-sources.md)

# Type mapping

Windows 1.0 primarily uses PostgreSQL text result format. The OID, typmod,
domain base type, and catalog metadata select the codec; an unknown type is
never silently narrowed.

| PostgreSQL | SQLite value | Policy |
|---|---|---|
| `bool` | INTEGER 0/1 | canonical `t`/`f` only |
| `int2/int4/int8` | INTEGER | checked parse, overflow → error |
| `numeric/decimal` | TEXT by default | exact lexical value, explicit special-value policy |
| `float4/float8` | REAL | NaN/Infinity only under the selected policy |
| `text/varchar/bpchar/name` | TEXT | UTF-8; NULL remains distinct from empty text |
| `bytea` | BLOB | exact hex decode; empty BLOB is preserved |
| `date/time/timestamp/timestamptz/interval` | TEXT | deterministic policy without locale parsing |
| `uuid/json/jsonb/xml` | TEXT | validated server representation |
| enum | TEXT | catalog label |
| domain | base mapping | domain OID/typmod remain in metadata |
| arrays/ranges/composite/unknown UDT | TEXT fallback | documented; DML only with a proven codec |

NULL is never converted to an empty TEXT or BLOB value. Numeric, bytea,
date/time, and spatial parsers use checked bounds. Malformed, truncated, or
overflowing input returns a SQLite error while preserving SQLSTATE when the
server supplied one.

DML parameters carry an explicit PostgreSQL OID and text/binary format.
`__vps_omit` distinguishes defaults from NULL; generated and identity ALWAYS
columns are protected by catalog policy.

## See Also

- [Table metadata](table-metadata.md) — OID/typmod discovery.
- [DML and identity](dml-identity.md) — parameter encoding and defaults.
- [Spatial](spatial.md) — geometry/geography codecs.
