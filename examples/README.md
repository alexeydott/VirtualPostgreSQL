# Примеры

Примеры не содержат credentials. Перед запуском зарегистрируйте безопасный
`credential_ref` в host provider либо замените его локальным non-production
connection mode, понимая последствия хранения `connstr` в SQLite schema.

| Файл | Назначение |
|---|---|
| `read-only.sql` | table/query sources, metadata и pushdown |
| `dml-transactions.sql` | keyed DML, defaults, optimistic identity и savepoint |
| `spatial.sql` | WKT/WKB/EWKT/EWKB configuration |
| `credential-provider.c` | минимальный ABI 1.0 provider skeleton |
