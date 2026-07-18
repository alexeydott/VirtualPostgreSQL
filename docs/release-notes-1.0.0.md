[← Previous: Platform support](platform-support.md) · [Back to README](../README.md) · [Next: Acceptance matrix →](windows-1.0-acceptance.md)

# VirtualPostgreSQL 1.0.0 — Windows

The first stable Windows release provides separate Win32/x64 SQLite loadable
extensions with public credential-provider ABI 1.0, including connection-scoped
provider registration and the Windows Credential Manager provider factory. The
runtime statically includes libpq 18.4, OpenSSL, zlib, and the private SQLite
materialization engine.

## Highlights

- PostgreSQL 15–18 table, view, and query sources plus catalog metadata functions.
- Async connect/query, single-row streaming, bounded pool, and secure cancellation.
- Conservative predicate/projection/IN/order/limit pushdown with local recheck.
- Exact scalar/bytea/date codecs, stable identity, keyed DML, and transactions.
- Query materialization `memory|temp`, metadata cache/integrity, and PostGIS.
- Default TLS `verify-full`, provider/WinCred secrets, and structured redaction.

## Public ABI freeze

- API version: `1.0.0` (`0x01000000`), calling convention `__cdecl`.
- Struct sizes Win32: header 16, config 112, lease 40, provider 48 bytes.
- Struct sizes x64: header 16, config 200, lease 64, provider 72 bytes.
- Approved exports: SQLite entry point, API/size queries,
  `virtualpostgresql_register_credential_provider`,
  `virtualpostgresql_wincred_provider`, and `virtualpostgresql_cancel`; no
  generic private-SQLite or internal WinCred exports.

## Limitations

Query sources are not a security sandbox; a least-privilege role is required.
Views, materialized views, foreign tables, and query sources are read-only.
Distributed transactions, DDL, CDC, COPY, and automatic spatial transforms are
not supported. Linux and Android are post-Windows goals; the stock Android
SQLite limitation is described in
[platform support](platform-support.md).

Complete executable matrix: [Windows 1.0 acceptance](windows-1.0-acceptance.md).

## See Also

- [Platform support](platform-support.md) — supported release matrix.
- [Building](building.md) — build and verification commands.
- [Acceptance matrix](windows-1.0-acceptance.md) — 132 criteria.
