# VirtualPostgreSQL

VirtualPostgreSQL is a Windows-first, portable-C SQLite (v. 3.44.0 or later) Virtual Table extension for PostgreSQL access without external client DLLs.
Currently, builds are available for both Win32 and x64 builds with either a statically linked dependencies.

## Features

* PostgreSQL 15–18; SQLite host 3.44.0 or later, module v4, and `xIntegrity`.
* Table and query metadata, exact scalar, `bytea`, and date codecs, and stable identity.
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
| Quality and release | [Static analysis](docs/static-analysis.md), [Sanitizers](docs/sanitizers.md), [Release notes](docs/release-notes-1.0.0.md), [Acceptance](docs/windows-1.0-acceptance.md), [Examples](examples/README.md) |

Linux and Android supports teoricaly but not are long-term development tracks. Stock Android SQLite builds generally do not permit loadable extensions; a future Android host must support either dynamic loading or static entry-point registration.

## License

See LICENSE and the dependency notices in
licenses/README.md.
