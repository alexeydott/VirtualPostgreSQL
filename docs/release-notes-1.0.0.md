[← Previous: Platform support](platform-support.md) · [Back to README](../README.md) · [Next: Acceptance matrix →](windows-1.0-acceptance.md)

# VirtualPostgreSQL 1.0.0 — Windows

Первый стабильный Windows release предоставляет отдельные Win32/x64 SQLite
loadable extensions с public credential-provider ABI 1.0. Runtime статически
включает libpq 18.4, OpenSSL, zlib и private SQLite materialization engine.

## Основные возможности

- PostgreSQL 15–18 table/view/query sources и catalog metadata functions.
- Async connect/query, single-row streaming, bounded pool и secure cancel.
- Conservative predicate/projection/IN/order/limit pushdown с local recheck.
- Exact scalar/bytea/date codecs, stable identity, keyed DML и transactions.
- Query materialization `memory|temp`, metadata cache/integrity и PostGIS.
- Default TLS `verify-full`, provider/WinCred secrets и structured redaction.

## Public ABI freeze

- API version: `1.0.0` (`0x01000000`), calling convention `__cdecl`.
- Struct sizes Win32: header 16, config 112, lease 40, provider 48 bytes.
- Struct sizes x64: header 16, config 200, lease 64, provider 72 bytes.
- Approved exports: SQLite entry point, API/size queries and
  `virtualpostgresql_cancel`; no generic private-SQLite exports.

## Ограничения

Query source не является security sandbox; требуется least-privilege role.
Views/materialized views/foreign tables и query sources read-only. Distributed
transactions, DDL, CDC, COPY и automatic spatial transform отсутствуют.
Linux/Android — post-Windows цели; stock Android SQLite limitation описан в
[platform support](platform-support.md).

Полная проверяемая матрица: [Windows 1.0 acceptance](windows-1.0-acceptance.md).

## See Also

- [Platform support](platform-support.md) — supported release matrix.
- [Building](building.md) — build and verification commands.
- [Acceptance matrix](windows-1.0-acceptance.md) — 132 criteria.
