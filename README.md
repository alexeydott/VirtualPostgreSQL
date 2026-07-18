# VirtualPostgreSQL

> SQLite Virtual Table extension на portable C11 для безопасного доступа к PostgreSQL без внешних client DLL.

VirtualPostgreSQL отображает PostgreSQL tables/views и проверенные read-only
`SELECT` как SQLite virtual tables. Windows 1.0 поддерживает async static libpq,
single-row streaming, conservative pushdown, keyed DML/transactions, PostGIS и
versioned metadata cache для Win32 и x64.

## Quick start

Соберите x64 Release и запустите 46 локальных tests:

```powershell
pwsh -NoProfile -File scripts/build-stage1.ps1 -Preset msvc-x64-release
```

Live PostgreSQL параметры передаются только process-local способом. Product
default остаётся `sslmode=verify-full`; no-SSL разрешён лишь явной policy.

## Пример

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

Values отправляются typed parameters, identifiers проходят отдельное quoting,
а PostgreSQL ACL и Row-Level Security продолжают действовать.

## Возможности Windows 1.0

- PostgreSQL 15–18; SQLite host 3.44.0+, module v4 и `xIntegrity`.
- Table/query metadata, exact scalar/bytea/date codecs и stable identity.
- Predicate/projection/IN/order/limit pushdown с local recheck.
- Bounded pool, independent cursors, cancellation и unknown-COMMIT safety.
- Query materialization `memory|temp`, PostGIS WKT/WKB/EWKT/EWKB.
- Static Win32/x64 runtime: без `libpq.dll`, OpenSSL DLL и zlib DLL.

## Документация

| Раздел | Руководства |
|---|---|
| Начало | [Building](docs/building.md), [Platform support](docs/platform-support.md), [Troubleshooting](docs/troubleshooting.md) |
| Подключение | [Credentials](docs/connection-credentials.md), [Provider ABI](docs/provider-abi.md), [Security](docs/security.md) |
| Runtime | [Client](docs/client-runtime.md), [Streaming/cancel](docs/streaming-cancellation.md) |
| Schema и types | [Table metadata](docs/table-metadata.md), [Type mapping](docs/type-mapping.md), [Metadata/cache](docs/metadata-functions-cache.md) |
| Query | [Query sources](docs/query-sources.md), [Read-only VTable](docs/read-only-vtable.md), [Planner](docs/planner-pushdown.md) |
| Запись | [DML/identity](docs/dml-identity.md), [Transactions](docs/transactions-savepoints.md) |
| Spatial | [PostGIS](docs/spatial.md) |
| Quality/release | [Static analysis](docs/static-analysis.md), [Sanitizers](docs/sanitizers.md), [Release notes](docs/release-notes-1.0.0.md), [Acceptance](docs/windows-1.0-acceptance.md), [Examples](examples/README.md) |

Linux и Android — долгосрочные post-Windows contours. Stock Android SQLite
обычно не разрешает loadable extensions; будущий Android host должен
поддерживать dynamic load либо static entry-point registration.

## License

See [LICENSE](LICENSE) and dependency notices in [licenses/README.md](licenses/README.md).
