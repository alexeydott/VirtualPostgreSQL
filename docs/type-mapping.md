[← Previous: Table metadata](table-metadata.md) · [Back to README](../README.md) · [Next: Query sources →](query-sources.md)

# Type mapping

Windows 1.0 преимущественно использует PostgreSQL text result format. OID,
typmod, domain base type и catalog metadata определяют codec; неизвестный тип
не получает silent narrowing.

| PostgreSQL | SQLite value | Политика |
|---|---|---|
| `bool` | INTEGER 0/1 | только canonical `t`/`f` |
| `int2/int4/int8` | INTEGER | checked parse, overflow → error |
| `numeric/decimal` | TEXT по умолчанию | exact lexical value, special policy explicit |
| `float4/float8` | REAL | NaN/Infinity только по выбранной policy |
| `text/varchar/bpchar/name` | TEXT | UTF-8, NULL отдельно от empty |
| `bytea` | BLOB | exact hex decode, empty BLOB сохранён |
| `date/time/timestamp/timestamptz/interval` | TEXT | deterministic policy, без locale parsing |
| `uuid/json/jsonb/xml` | TEXT | validated server representation |
| enum | TEXT | catalog label |
| domain | base mapping | domain OID/typmod сохраняются в metadata |
| arrays/ranges/composite/unknown UDT | TEXT fallback | documented, DML только при доказанном codec |

NULL никогда не превращается в empty TEXT/BLOB. Numeric, bytea, date/time и
spatial parsers используют checked bounds; malformed/truncated/overflow input
возвращает SQLite error с сохранённым SQLSTATE, если он получен от server.

DML parameters имеют explicit PostgreSQL OID и text/binary format. Defaults
отличаются от NULL через `__vps_omit`; generated и identity ALWAYS columns
защищены catalog policy.

## See Also

- [Table metadata](table-metadata.md) — OID/typmod discovery.
- [DML and identity](dml-identity.md) — parameter encoding и defaults.
- [Spatial](spatial.md) — geometry/geography codecs.
