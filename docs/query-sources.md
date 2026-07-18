[← Previous: Type mapping](type-mapping.md) · [Back to README](../README.md) · [Next: Read-only Virtual Table →](read-only-vtable.md)

# Query sources

A query source maps the result of one `SELECT` or `WITH ... SELECT` statement
as a read-only virtual table. An inline query passes a bounded lexical scanner,
then PostgreSQL prepare/describe through an outer wrapper. Production
deployments should prefer a versioned `query_profile` resolved by a host,
protected-configuration, environment, or named-registry provider.

## Validation boundary

Before execution, the extension:

1. validates UTF-8, embedded NUL, size, quotes, comments, dollar quotes, and the
   single-statement boundary;
2. rejects unresolved `$n`, DML/DDL, data-modifying CTEs, `COPY`, `CALL`, `DO`,
   transaction/session control, `LISTEN`/`NOTIFY`, locking SELECT, and
   `SELECT INTO`;
3. prepares and describes, without executing,
   `SELECT * FROM (<query>) AS vps_validation LIMIT 0`;
4. requires non-empty unique result aliases using ASCII case-insensitive
   canonical comparison;
5. permits `key_columns` and `query_indexes` only over canonical result columns;
6. scans inside `BEGIN READ ONLY` with transaction-local controlled
   `search_path`, `statement_timeout`, `lock_timeout`, and extension-side
   row/byte/deadline limits;
7. closes the boundary only with `COMMIT` or `ROLLBACK`.

The inner query is never rewritten. The planner adds predicates, ordering, and
limits only to the outer execution wrapper.

## Security limitation

Query validation is defense in depth, not a PostgreSQL sandbox. An accepted
`SELECT` can call a function with external side effects that a read-only
transaction cannot undo. Production configuration must use a dedicated
least-privilege role, schema-qualified object names, a controlled `search_path`,
revoked unnecessary `EXECUTE` privileges, and approved query profiles. The
calling application must explicitly warn users when inline queries are enabled.

Raw query text, profile content, and values never enter normal logs, errors, or
fingerprints. Debug SQL is allowed only by the existing compile-time
`VPS_DEBUG` and runtime `debug` gate; Release and RelWithDebInfo builds reject
the field.

## Result identity and indexes

`key_columns` declares a validated logical identity for read-only planning
only; a query source never gains DML support. `query_indexes` uses the grammar
`name=column[,column...][;...]`, preserves column order, and does not claim a
remote or unique index. Unique planning may rely only on validated
`key_columns`.

The query fingerprint includes the normalized query hash/profile version,
ordered aliases and OID/typmod/origin/collation metadata, spatial policy, keys,
query indexes, materialization mode, and wrapper/codec versions. A change to
any contract-relevant component is metadata drift.

## Query materialization

`query_materialization=off|memory|temp` controls a lazy snapshot only for
`source=query`; the default `off` retains single-row remote streaming. `memory`
builds an immutable private in-memory SQLite database. `temp` builds a private
database in `%LOCALAPPDATA%\VirtualPostgreSQL\Temp` with an owner-only access
control list (ACL), an in-memory rollback journal, and delete-on-close cleanup.
Host SQLite handles and allocators are never passed to private SQLite.

The first scan executes the validated source query exactly once, transfers
already-decoded portable SQLite values, and builds non-unique physical indexes
from `query_indexes`. The candidate becomes visible only after the terminal
remote result, all limit checks, local index construction, and private commit.
A failed, late-error, out-of-memory, temporary-file, or index build is cleaned
up without partial publication; the next caller may safely retry the build. A
published generation is immutable and has no TTL, automatic refresh, or host
shadow rows.

Subsequent cursors obtain a reference-counted lease and evaluate exact
predicates, projection, compatible ordering, and consumed limit/offset locally.
`query_indexes` never creates a uniqueness claim: `SQLITE_INDEX_SCAN_UNIQUE`
remains valid only for fully constrained, validated `key_columns`. Refreshing a
snapshot requires disconnect/reconnect or virtual-table recreation; there is no
hidden refresh.

The stage gate measures at least three samples of 100 probes. Remote executions
must fall from `N` to `1`, the physical local index is confirmed through private
`EXPLAIN QUERY PLAN`, and p50 must improve by at least 10%. Bulk, equivalence,
and performance contours run on the local no-SSL stand; the quota-limited TLS
stand is never used for bulk data.

## See Also

- [Read-only Virtual Table](read-only-vtable.md) — SQLite callback path.
- [Planner](planner-pushdown.md) — exact pushdown policy.
- [Type mapping](type-mapping.md) — result codecs.
