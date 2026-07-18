# DML and stable identity

Table sources may opt into keyed writes with `mode=rw`. Query sources, views,
materialized views, foreign tables, unsafe inheritance layouts and relations
without a catalog-proven primary/unique key remain read-only. PostgreSQL catalog
metadata is the authority for key, generated-column and identity-column policy;
`ctid` and scan position are never write identities.

Rows with a single integral key use the SQLite rowid only when optimistic
locking is disabled. Composite keys and optimistic modes expose a hidden
`__vps_identity` BLOB. The token is versioned, bounded, relation-scoped and
length-prefixed, and carries typed original key fields plus the optional
original version. UPDATE and DELETE decode it strictly and bind the original
key with `IS NOT DISTINCT FROM` predicates.

## INSERT defaults and NULL

SQLite's virtual-table callback does not distinguish an omitted INSERT column
from an explicit `NULL`. VirtualPostgreSQL therefore treats ordinary visible
values as explicit, including NULL, and provides the hidden `__vps_omit`
control column for defaults:

```sql
INSERT INTO remote_table(payload, __vps_omit)
VALUES ('value', 'id,created_at,generated_value');

INSERT INTO remote_table(__vps_omit) VALUES ('*'); -- DEFAULT VALUES
```

Names are exact, comma-separated remote column names. Generated columns and
`GENERATED ALWAYS AS IDENTITY` columns must be omitted. Identity BY DEFAULT may
be supplied. All SQL is identifier-quoted and values are extended-protocol
parameters; bytea uses binary format and numeric retains its text form.

UPDATE uses `sqlite3_vtab_nochange`/`sqlite3_value_nochange` to exclude columns
not named by the statement. Key changes are supported because the predicate
always uses the original identity and RETURNING publishes the new key.

## Optimistic locking

`optimistic_lock=column` requires a `version_column` that is NOT NULL, exact and
writable. Its original value is carried in the identity predicate and the new
value is returned. `optimistic_lock=xmin` is opt-in and uses PostgreSQL `xmin`
only as a short-lived conflict token. It is not a business key, is not stable
across dump/restore, and is not claimed to be a globally unique transaction ID.

Keyed DML accepts exactly one RETURNING row and exactly one parsed
`PQcmdTuples` affected row. Zero rows under optimistic locking are conflicts;
more than one row is an invariant failure. DML is never retried automatically.
