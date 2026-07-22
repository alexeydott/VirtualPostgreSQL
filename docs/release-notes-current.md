[← Previous: Platform support](platform-support.md) · [Back to README](../README.md) · [Next: Acceptance matrix →](windows-current-acceptance.md)

# Current Windows release

VirtualPostgreSQL currently provides separate Win32/x64 SQLite loadable
extensions with a public credential-provider ABI, connection-scoped provider
registration, and a Windows Credential Manager provider factory. The runtime
statically includes the pinned libpq, OpenSSL, zlib, and private SQLite
dependencies.

## Highlights

- PostgreSQL 15–18 table, view, and query sources plus catalog metadata functions.
- Async connect/query, single-row streaming, bounded pool, and secure cancellation.
- Conservative predicate/projection/IN/order/limit pushdown with local recheck.
- Exact scalar/bytea/date codecs, stable identity, keyed DML, and transactions.
- Query materialization `memory|temp`, metadata cache/integrity, and PostGIS.
- Default TLS `verify-full`, provider/WinCred secrets, and structured redaction.

## Current public ABI

- Calling convention: `__cdecl`; API compatibility is reported by
  `virtualpostgresql_api_version()` and the constants in the public header.
- Struct sizes Win32: header 16, config 112, lease 40, provider 48 bytes.
- Struct sizes x64: header 16, config 200, lease 64, provider 72 bytes.
- Query-profile sizes Win32: lease 56, provider 48 bytes; x64: lease 80,
  provider 72 bytes.
- Approved exports: SQLite entry point, API/provider size queries,
  `virtualpostgresql_register_credential_provider`,
  `virtualpostgresql_register_query_profile_provider`,
  `virtualpostgresql_wincred_provider`, and `virtualpostgresql_cancel`; no
  generic private-SQLite or internal provider exports.

## Limitations

Query sources are not a security sandbox; a least-privilege role is required.
Views, materialized views, foreign tables, and query sources are read-only.
Distributed transactions, DDL, CDC, COPY, and automatic spatial transforms are
not supported. Linux and Android are post-Windows goals; the stock Android
SQLite limitation is described in
[platform support](platform-support.md).

The complete executable matrix is maintained in the
[current Windows acceptance page](windows-current-acceptance.md).

## See Also

- [Platform support](platform-support.md) — supported release matrix.
- [Building](building.md) — build and verification commands.
- [Acceptance matrix](windows-current-acceptance.md) — 132 criteria.
