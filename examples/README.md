# Examples

These examples contain no passwords, tokens, working endpoints, or other
credentials. Replace names such as `app/reporting` only in a private runtime
configuration. Do not add the resulting values to source control.

| File | Purpose |
|---|---|
| `read-only.sql` | Table and approved-query sources, metadata, and pushdown |
| `connection-modes.sql` | Safe shapes for `credential_ref`, `service`, `profile`, and compatibility `connstr` |
| `dml-transactions.sql` | Keyed DML, defaults, optimistic identity, and a savepoint |
| `spatial.sql` | WKT/WKB/EWKT/EWKB configuration |
| `credential-provider.c` | Minimal host-owned credential provider callbacks |
| `windows-credential-provider.c` | Registration of the built-in Windows Credential Manager adapter |
| `query-profile-provider.c` | Approved-query provider, revision, registration, and lease release |

## Choosing a connection example

Prefer `credential_ref`: the SQLite schema contains only an opaque reference,
and the host supplies credentials through a database-scoped provider. On
Windows, `windows-credential-provider.c` shows how to initialize the built-in
Generic Credential adapter and register it after loading the extension.

`service` delegates configuration to libpq's service mechanism. `profile`
reads bounded `VPS_PROFILE_<NAME>_<FIELD>` process-environment values. Neither
mode makes it safe to commit its external configuration or secrets.

`connstr` is included for compatibility and local diagnostics. The example has
no password, uses `sslmode=verify-full`, and creates a `TEMP` virtual table.
Never embed a production password in a connection string: persistent virtual
table SQL is stored in `sqlite_schema` and is commonly copied into backups.

## Provider integration

The C snippets are intended to be compiled into the SQLite host. Obtain the
exported functions from the already loaded `virtualpostgresql.dll` (or link an
import library produced by your build), then register providers on the same
`sqlite3*` connection that loaded the extension. Provider callback state must
remain valid until that SQLite connection closes.

Both provider interfaces use a lease contract. After a successful `resolve`,
VirtualPostgreSQL copies and validates the returned fields and invokes
`release` exactly once, including later validation or allocation failures.
Callbacks must not call SQLite APIs and must never log connection fields, raw
queries, or references.
