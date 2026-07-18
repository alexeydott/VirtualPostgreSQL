[← Previous: Sanitizers](sanitizers.md) · [Back to README](../README.md) · [Next: Platform support →](platform-support.md)

# Troubleshooting

| Симптом | Проверка | Безопасное действие |
|---|---|---|
| DLL не загружается | SQLite ≥3.44, x86/x64 совпадают | запустить `test-windows-binary.ps1` |
| `authentication failed` | role/provider/service | обновить secret source, не печатать connstr |
| TLS/certificate error | hostname, CA, `sslmode` | исправить trust; не снижать mode автоматически |
| `SQLITE_SCHEMA` | relation OID/fingerprint drift | recreate либо совместимый `schema_policy=refresh` |
| `SQLITE_BUSY` | active stream/transaction participant | завершить cursor и повторить на уровне app |
| `SQLITE_INTERRUPT` | deadline или cancel | проверить timeout и host interrupt policy |
| commit outcome unknown | connection lost во время COMMIT | считать failure; проверить durable business key |
| PostGIS unavailable | extension/schema discovery | установить/разрешить PostGIS или убрать spatial mode |

В debug build server primary error проходит bounded redaction. SQL можно
включить только сочетанием `VPS_DEBUG` и runtime `debug`; values и conninfo всё
равно не логируются. Не прикладывайте к bug report environment dump, database
URL, certificate/private key, row data или SQLite schema с `connstr`.

Для воспроизводимого отчёта укажите architecture, SQLite version, PostgreSQL
major, extension API version, operation/phase, SQLite code и SQLSTATE. Затем
запустите подходящий gate из [Building](building.md).

## See Also

- [Security](security.md) — допустимые diagnostic fields.
- [Building](building.md) — локальные verification commands.
- [Transactions](transactions-savepoints.md) — ambiguous COMMIT recovery.
