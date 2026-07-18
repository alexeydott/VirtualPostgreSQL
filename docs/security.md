[← Previous: Provider ABI](provider-abi.md) · [Back to README](../README.md) · [Next: Client runtime →](client-runtime.md)

# Security

## Connection

- The product default is `sslmode=verify-full`; downgrade requires an explicit
  configuration choice.
- `channel_binding=require` fails closed when channel binding is not confirmed.
- Prefer a host credential provider or Windows Credential Manager as the secret
  source. `connstr` is a compatibility fallback that may remain in the SQLite
  schema and is therefore not recommended for production secrets.
- The metadata cache never masks authentication, TLS, or configuration errors.

## PostgreSQL role

Use a dedicated role without `SUPERUSER`, `CREATEDB`, or `CREATEROLE`. Grant
only the required `USAGE`, `SELECT`, and DML privileges. VirtualPostgreSQL does
not bypass PostgreSQL access-control lists (ACLs) or Row-Level Security (RLS),
and it is not an SQL sandbox. Use approved profiles and schema-qualified names
for query sources.

## SQL boundary

Internal SQL qualifies `pg_catalog`; values are passed only as parameters, and
identifiers follow separate validation and quoting routines. The controlled
`search_path` is part of the canonical connection identity. The query scanner
rejects multiple statements, DML/DDL, locking SELECT, and session control, but
it does not replace a least-privilege role.

## Logs and artifacts

Passwords, tokens, private keys, connection strings/effective conninfo, bound
values, row payloads, and WKT/WKB are forbidden. A PostgreSQL primary message
is permitted only in a bounded, redacted debug field; DETAIL, HINT, and CONTEXT
are never extracted. Raw SQL is available only with compile-time `VPS_DEBUG`
and runtime `debug`; Release builds fail closed.

Report vulnerabilities to the repository owner through a private channel. Do
not publish credentials, crash dumps, or production schemas in an issue.

## See Also

- [Connection and credentials](connection-credentials.md) — TLS and secret modes.
- [Provider ABI](provider-abi.md) — lifetime and secure release.
- [Troubleshooting](troubleshooting.md) — safe failure diagnostics.
