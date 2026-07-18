# Examples

The examples do not contain credentials. Before running them, register a secure
`credential_ref` with your hosting provider or replace it with a local connection mode not intended for a production environment,
keeping in mind the implications of storing `connstr` in the SQLite schema.

| File | Purpose |
|---|---|
| `read-only.sql` | table/query sources, metadata, and pushdown |
| `dml-transactions.sql` | DML with keys, defaults, optimistic identity, and savepoint |
| `spatial.sql` | WKT/WKB/EWKT/EWKB configuration |
| `credential-provider.c` | minimal ABI 1.0 provider skeleton |