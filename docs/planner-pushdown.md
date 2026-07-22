[← Previous: Read-only Virtual Table](read-only-vtable.md) · [Back to README](../README.md) · [Next: Streaming and cancellation →](streaming-cancellation.md)

# Planner and safe pushdown

Currently, a bounded planner sits between SQLite `xBestIndex` and `xFilter`.
It produces a printable, format-tagged compiled plan. Every plan carries the
source fingerprint and fixed-width counts; `xFilter` rejects unknown formats,
fingerprint drift, truncation, malformed fields and bounds
violations before acquiring a PostgreSQL connection.

## Exactness policy

The current planner is deliberately conservative:

- integer and boolean comparisons can be pushed for compatible SQLite values;
- canonical lowercase UUID equality and byte-exact `bytea` equality can be
  pushed;
- `IS NULL` and `IS NOT NULL` are exact;
- text with unproven collation, numeric dynamic coercion, floating-point NaN,
  JSON and custom types remain local SQLite checks;
- `omit=1` is used only when the remote predicate is proven equivalent;
- mixed or oversized `IN` inputs fall back to SQLite recheck, and the pushed
  list is bounded to 4096 items.

Values are copied into cursor-owned parameter storage and sent through libpq
parameter arrays. They are never interpolated into SQL or logged.

## Projection and ordering

`colUsed` drives the remote projection. Key columns and columns needed by
pushed predicates or ordering are retained automatically. SQLite's bit 63 rule
is handled conservatively by projecting every used column at index 63 or
higher. The cursor owns an explicit logical-to-remote map, so `xColumn` and row
identity do not depend on projection order.

`ORDER BY` is consumed only when every term has an exact ordering codec. The
generated PostgreSQL SQL states SQLite-compatible NULL placement explicitly:

- ascending: `NULLS FIRST`;
- descending: `NULLS LAST`.

Unproven text/collation ordering stays in SQLite.

## LIMIT, OFFSET and query sources

`LIMIT` and `OFFSET` are pushed only when no earlier local recheck can discard
rows. Negative SQLite `LIMIT` means no remote limit; negative `OFFSET` is
normalized to zero. Query sources retain their validated inner query unchanged
and receive projection, predicates and ordering only through an outer wrapper.

## Cost and unique lookup

A full exact equality match for every configured stable key part is advertised
as `SQLITE_INDEX_SCAN_UNIQUE` with one estimated row. Other plans use bounded,
deterministic estimates from available row/page/index-prefix metadata and a
fixed fallback. `xBestIndex` never executes remote `EXPLAIN`.

Planner decisions are observable through structured debug fields containing
only the plan format, flags, counts, estimated cost/rows and source fingerprint. Raw
values and connection information are excluded.

## See Also

- [Read-only Virtual Table](read-only-vtable.md) — planner callback integration.
- [Streaming and cancellation](streaming-cancellation.md) — execution lifecycle.
- [Query sources](query-sources.md) — wrapper and local materialization.
