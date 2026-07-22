# VirtualPostgreSQL

VirtualPostgreSQL is a SQLite extension that exposes PostgreSQL tables, views, materialized views, foreign tables, approved query results, and PostGIS data as SQLite virtual tables.
Once loaded, remote data can be accessed through standard SQLite SQL. It can be queried, filtered, ordered, joined with local tables, streamed or materialized, and, when a stable key is available - modified using INSERT, UPDATE, and DELETE within transactions and savepoints.

Currently, the extension is distributed as a single virtualpostgresql.dll for Win32 and Win64. It includes the required PostgreSQL client, TLS, compression, and private SQLite components, so no separate libpq or supporting client DLLs are required.

VirtualPostgreSQL is primarily intended for Windows applications that already use SQLite but also need secure, transactional access to PostgreSQL and PostGIS data without implementing a separate database access layer or middleware. It supports asynchronous connections, streaming reads, query pushdown, credential providers, Windows Credential Manager, configurable TLS verification, cancellation, connection pooling, and schema-integrity checks.

## Features

* PostgreSQL 15–18; SQLite host 3.44.0 or later, module v4, and `xIntegrity`.
* Table and query metadata, exact scalar, `bytea`, and date codecs, and stable identity.
* Database-scoped query-profile providers for centrally managed definitions.
* Predicate, projection, `IN`, ordering, and limit pushdown with local rechecks.
* Bounded connection pool, independent cursors, cancellation, and unknown-COMMIT safety.
* Query materialization using `memory` or `temp`; PostGIS WKT, WKB, EWKT, and EWKB.
  
## Quick Start

Build the x64 Release configuration and run 46 local tests:

```powershell
pwsh -NoProfile -File scripts/build-stage1.ps1 -Preset msvc-x64-release
```

Live PostgreSQL connection parameters are supplied only through process-local
mechanisms. The product default remains `sslmode=verify-full`; connections
without SSL are permitted only when explicitly allowed by policy.

## Example

```sql
CREATE VIRTUAL TABLE temp.orders USING VirtualPostgreSQL(
  credential_ref='app/reporting', source=table,
  schema='reporting', table='orders', mode=ro,
  key_columns='order_id'
);

SELECT order_id, created_at
FROM orders
WHERE order_id IN (101,102)
ORDER BY order_id LIMIT 10;
```

Values are sent as typed parameters, identifiers are quoted separately, and
PostgreSQL ACLs and Row-Level Security remain in effect.

## Documentation

| Section             | Guides                                                                                                                                                                                                   |
| ------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Getting started     | [Building](docs/building.md), [Platform support](docs/platform-support.md), [Troubleshooting](docs/troubleshooting.md)                                                                                   |
| Connectivity        | [Credentials](docs/connection-credentials.md), [Provider ABI](docs/provider-abi.md), [Security](docs/security.md)                                                                                        |
| Runtime             | [Client](docs/client-runtime.md), [Streaming/cancel](docs/streaming-cancellation.md)                                                                                                                     |
| Schema and types    | [Table metadata](docs/table-metadata.md), [Type mapping](docs/type-mapping.md), [Metadata/cache](docs/metadata-functions-cache.md)                                                                       |
| Queries             | [Query sources](docs/query-sources.md), [Read-only VTable](docs/read-only-vtable.md), [Planner](docs/planner-pushdown.md)                                                                                |
| Data modification   | [DML/identity](docs/dml-identity.md), [Transactions](docs/transactions-savepoints.md)                                                                                                                    |
| Spatial data        | [PostGIS](docs/spatial.md)                                                                                                                                                                               |
| Quality and release | [Static analysis](docs/static-analysis.md), [Sanitizers](docs/sanitizers.md), [Release notes](docs/release-notes-current.md), [Acceptance](docs/windows-current-acceptance.md), [Examples](examples/README.md) |

Currently, Windows is the only supported runtime. Linux and Android remain
long-term development tracks. Stock Android SQLite builds generally do not
permit loadable extensions; a future Android host must support either dynamic
loading or static entry-point registration.

## License

See LICENSE and the dependency notices in
licenses/README.md.
