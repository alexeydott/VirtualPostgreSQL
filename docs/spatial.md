[← Previous: Transactions](transactions-savepoints.md) · [Back to README](../README.md) · [Next: Metadata and cache →](metadata-functions-cache.md)

# PostGIS и spatial values

PostGIS поддержка optional и определяется только через PostgreSQL catalogs.
Extension schema может быть нестандартной; вызовы spatial functions всегда
schema-qualified по доверенному результату discovery.

| Режим | SQLite representation | SRID |
|---|---|---|
| `geometry=wkt` | TEXT WKT | typmod или explicit `srid=` |
| `geometry=wkb` | BLOB WKB | typmod или explicit `srid=` |
| `geometry=ewkt` | TEXT EWKT | embedded/typmod должны согласоваться |
| `geometry=ewkb` | BLOB EWKB | embedded/typmod должны согласоваться |

Поддерживаются `geometry` и `geography`, EMPTY и NULL остаются различными.
Structural validators проверяют endian/type/dimensions/SRID/count/nesting,
finite coordinates и exact payload length до server send. Extension не меняет
ось координат, не выполняет `ST_Transform` и не подменяет SRID.

SpatiaLite output в Windows 1.0 явно unavailable. Запись доступна только для
writable table source с согласованными catalog capabilities; query source,
view, materialized view и foreign table остаются read-only.

Примеры находятся в [examples/spatial.sql](../examples/spatial.sql).

## See Also

- [Type mapping](type-mapping.md) — scalar codec policy.
- [Query sources](query-sources.md) — result overrides.
- [Security](security.md) — payload redaction.
