[← Previous: Planner and pushdown](planner-pushdown.md) · [Back to README](../README.md) · [Next: DML and identity →](dml-identity.md)

# Streaming, cancellation and concurrency

Currently, VirtualPostgreSQL publishes PostgreSQL rows through a narrow cursor state
machine: `NEW → OPEN → FILTERING ↔ WAITING → ROW_READY → EOF`. Cancellation
uses the explicit `CANCELLING` state and invariant failures use `FAILED`; every
cursor terminates in `CLOSED`. A row becomes visible to SQLite only after all
projected values and hidden identity data have been decoded successfully.
The borrowed libpq result remains alive until the next step or cursor close.

Late PostgreSQL errors are returned by the later `sqlite3_step()` call. Rows
already observed by the caller are not retracted, so callers must discard the
partial statement result when its terminal result is not `SQLITE_DONE`.

## Cancellation and cleanup

The public `virtualpostgresql_cancel(sqlite3*)` export requests cancellation of
all active VirtualPostgreSQL cursors owned by that SQLite database handle. It is
safe with a serialized SQLite connection and returns one of the public
`VPS_CANCEL_*` status constants. SQLite interruption, monotonic statement
deadlines and the host API all enter the same secure libpq cancellation path.

Only the libpq 18 secure cancel API is used. A successful cancel is drained to
the terminal `57014`, mapped to `SQLITE_INTERRUPT`, and checked before pool
reuse. Timeout, protocol damage, incomplete drain, early close without a clean
terminal state, or ambiguous connection state destroys the lease. Current-row
values, parameters, projection arrays and identity storage have idempotent
cleanup.

## Limits

Checked counters reject overflow and one-over-boundary inputs before row
publication. The cursor enforces result-row, result-byte, column-byte,
query-byte, parameter-byte, identity-byte and `IN` limits, plus reserved spatial
point/depth defaults. Result-byte and spatial defaults are architecture-aware;
the x86 contour uses smaller bounds than x64.

## Current verification

`scripts/ci/run-stage9.ps1` builds MSVC Win32 Debug, MSVC x64 Release, clang-cl
x64 Debug and clang-cl x64 ASan. The optional local stand runs the data-heavy
checks without TLS: a deterministic one-million-row fake-backend stream, a
real PostgreSQL streaming sample with first-row and peak-RSS measurements,
eight simultaneous cursors over 1,000 reset cycles, early close, late error,
explicit cancel, pool reuse, concurrent credential resolution and controlled
network loss before and after row publication.

The TLS stand is deliberately compact. It creates three idempotent control rows
and their indexes, validates them through the async adapter and read-only virtual
table, and does not run the bulk stream or cursor-cycle matrix. All credentials
are process-local environment variables; scripts never persist or print them.

## See Also

- [Client runtime](client-runtime.md) — async adapter capabilities.
- [Planner](planner-pushdown.md) — compiled execution plan.
- [DML and identity](dml-identity.md) — write-side ownership.
