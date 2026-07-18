# Transactions and savepoints

VirtualPostgreSQL maps SQLite transaction callbacks to one PostgreSQL
transaction pinned to the owning `sqlite3*` database context. The first
writable participant acquires the connection and sends `BEGIN`; later virtual
tables may join only when their canonical connection identity and generation
match exactly. A transaction never spans PostgreSQL identities or connections.

## Callback mapping

| SQLite callback | PostgreSQL action |
|---|---|
| `xBegin` | `BEGIN`, then the configured isolation and read-only policy |
| `xSync` | Validate that no stream or failed command remains |
| `xCommit` | `COMMIT` on the pinned connection |
| `xRollback` | `ROLLBACK` on the pinned connection |
| `xSavepoint(N)` | `SAVEPOINT vps_N` |
| `xRollbackTo(N)` | `ROLLBACK TO SAVEPOINT vps_N` |
| `xRelease(N)` | `RELEASE SAVEPOINT vps_N` |

Savepoint names are generated internally from the SQLite level; user input is
never interpolated. Nested levels are tracked in order. Rollback-to discards
deeper levels, while release validates and removes the addressed suffix.

The optional `isolation` argument accepts `read_committed` (default),
`repeatable_read`, or `serializable`. `transaction_read_only` defaults to false.
Both options are part of canonical identity, so participants with incompatible
policies cannot share a transaction.

## Failed and busy states

`PQTRANS_INERROR` is a transaction state, not a broken connection. After a
server error, new DML and scans are rejected until SQLite requests a valid
rollback-to or full rollback. The recovery command is sent on the same pinned
connection; there is no hidden retry or reconnect.

Because libpq permits only one active result stream per connection, a live
cursor makes conflicting DML, savepoint, sync, commit, and second-cursor work
return deterministic busy. EOF and cursor close release the stream gate.

## Ambiguous transaction outcome

If the connection is lost or a deadline/cancellation is observed while ending
a transaction, the lease is destroyed and the coordinator enters a terminal
ambiguous state. In particular, an unknown `COMMIT` result is never reported as
success and is never retried automatically.

The host must reconcile externally before repeating business work:

1. Treat the SQLite operation as failed with unknown remote outcome.
2. Open a fresh connection and inspect application state using a durable
   idempotency key or business identifier—not the failed connection.
3. Decide at the application layer whether the intended change is already
   committed, should be compensated, or can be submitted as a new operation.
4. Close or reset the affected SQLite database context before further virtual
   table transactions.

VirtualPostgreSQL provides no distributed transaction coordinator and makes no
atomicity claim across different PostgreSQL identities or ordinary SQLite
tables.
