[← Previous: Provider ABI](provider-abi.md) · [Back to README](../README.md) · [Next: Client runtime →](client-runtime.md)

# Безопасность

## Подключение

- Product default — `sslmode=verify-full`; downgrade выполняется только явно.
- `channel_binding=require` обязан завершиться fail-closed, если binding не
  подтверждён.
- Предпочтительный secret source — host credential provider или Windows
  Credential Manager. `connstr` — совместимый fallback, который может остаться
  в SQLite schema и поэтому не рекомендуется для production secrets.
- Metadata cache никогда не маскирует auth, TLS или configuration errors.

## PostgreSQL role

Используйте отдельную роль без `SUPERUSER`, `CREATEDB` и `CREATEROLE`. Выдавайте
только необходимые `USAGE`/`SELECT`/DML privileges. VirtualPostgreSQL не
обходит PostgreSQL ACL и Row-Level Security (RLS) и не является SQL sandbox.
Для query sources используйте утверждённые profiles и schema-qualified names.

## SQL boundary

Internal SQL квалифицирует `pg_catalog`; values передаются только параметрами,
а identifiers проходят отдельные validation/quoting routines. Controlled
`search_path` является частью canonical connection identity. Query scanner
отклоняет multiple statements, DML/DDL, locking SELECT и session control, но
не заменяет least-privilege role.

## Logs и artifacts

Запрещены password/token/private key, connstr/effective conninfo, bound values,
row payload и WKT/WKB. PostgreSQL primary message допускается только в bounded
redacted debug field; DETAIL/HINT/CONTEXT не извлекаются. Raw SQL доступен лишь
при compile-time `VPS_DEBUG` и runtime `debug`; Release fail-closed.

О найденной уязвимости сообщайте владельцу репозитория приватным каналом; не
публикуйте credential, crash dump или production schema в issue.

## See Also

- [Connection and credentials](connection-credentials.md) — TLS и secret modes.
- [Provider ABI](provider-abi.md) — lifetime и secure release.
- [Troubleshooting](troubleshooting.md) — безопасная диагностика отказов.
