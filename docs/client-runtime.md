[← Previous: Connection credentials](connection-credentials.md) · [Back to README](../README.md) · [Next: Table metadata →](table-metadata.md)

# Client runtime capabilities

VirtualPostgreSQL pins static libpq 18.4. The production adapter exposes only
capabilities whose required symbols are present in that pinned dependency and
whose implementation is non-blocking.

| Client capability | Required libpq 18.4 API | Runtime policy |
|---|---|---|
| Connect | `PQconnectStartParams`, `PQconnectPoll` | Monotonic deadline and socket wait |
| Prepare/describe | `PQsendPrepare`, `PQsendDescribePrepared` | Extended protocol only |
| Execute/fetch | `PQsendQueryParams`, `PQsendQueryPrepared`, `PQsetSingleRowMode` | Full terminal drain; row buffers are borrowed |
| Ping/reset | async query API plus `PQtransactionStatus`, `PQpipelineStatus` | Idle/no-pipeline/no-pending preflight; baseline reapplied |
| Cancel | `PQcancelCreate`, `PQcancelStart`, `PQcancelPoll`, `PQcancelSocket`, `PQcancelFinish` | Separate deadline; reuse only after a drained `57014` terminal result |

The deprecated `PQgetCancel` and `PQcancel` APIs are forbidden. A failed,
timed-out, interrupted, partially drained, transaction-active, pipeline-active,
or otherwise ambiguous operation makes the connection dirty and therefore
ineligible for return to the pool.

Retry is a portable policy decision, not an implicit libpq behavior. It is
allowed only for a classified transient connection failure on an idempotent
read before a transaction, row publication, or snapshot publication, and is
bounded by three attempts and capped exponential backoff.

## Debug diagnostics

PostgreSQL `MESSAGE_PRIMARY` is available as a bounded structured field only
at runtime log level `debug`. Quoted spans, control bytes and non-ASCII bytes
are replaced before emission; sensitive-token detection may redact the entire
field. `DETAIL`, `HINT`, `CONTEXT`, internal query and bound values are never
requested from libpq.

Raw statement SQL is present only in compile-time `VPS_DEBUG` builds and only
in `debug` events. Release and RelWithDebInfo builds reject the SQL field even
when runtime log level is `debug`. Parameter values remain excluded in every
build configuration.

## See Also

- [Connection credentials](connection-credentials.md) — identity, TLS and baseline policy.
- [Table metadata](table-metadata.md) — async catalog statements and snapshot ownership.
