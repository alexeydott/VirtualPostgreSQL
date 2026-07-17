# Stage 0 libpq prototype findings

This document records the contracts proved by the disposable Stage 0 programs. It is not a production API specification. Raw evidence is generated under the ignored `build/prototype-evidence/` directory and contains no connection strings, credentials, SQL text, bound values, result rows, or spatial payloads.

## Reproducible contour

- PostgreSQL/libpq 18.4, OpenSSL 3.5.7, and zlib 1.3.2 are pinned by `deps/versions.json` with upstream archive hashes and license metadata.
- Win32 and x64 Release/Debug builds use the supplied Visual Studio 2022 environment entry points and the static MSVC runtime (`/MT` or `/MTd`).
- libpq is statically linked with SSL, UTF-8, SCRAM, async query, single-row mode, and modern secure-cancel APIs available. NLS and PL/Tcl are disabled for this dependency contour. Tcl remains a later SQLite test-harness prerequisite, not a libpq prerequisite.
- Import inspection of all seven prototype executables found no runtime dependency on libpq, OpenSSL, or zlib DLLs. Deliberate wrong-architecture, wrong-runtime, missing-feature, and hash/version mismatch probes fail closed.

## Proven runtime contracts

### Async connection

`PQconnectStartParams` and `PQconnectPoll` operate as a nonblocking state machine driven by socket readiness and a `GetTickCount64` monotonic deadline. Success, authentication failure, refused endpoint, stalled handshake timeout, pre-signalled interrupt, and partial initialization were exercised on Win32 and x64. Every path calls `PQfinish` exactly once when a connection object exists.

The available stand accepts only an explicit local `sslmode=disable` setting. A successful connection to that stand is not evidence for the product TLS `verify-full` gate. The initially supplied role spelling was rejected by authentication; the corrected PostgreSQL role was used for successful stand tests. No credential was persisted.

### Extended query and binary values

- Async prepare, describe, and execute were exercised for zero and two parameters, explicit `int4` and `bytea` OIDs, text/binary formats, field names, OIDs, typmods, and result formats.
- Parameter-count mismatch (`08P01`) and invalid-OID preparation failure occur before row publication.
- `bytea` distinguishes NULL from empty data and round-trips embedded NUL plus 7-byte, 65,535-byte, 65,536-byte, and 1 MiB payloads byte-for-byte. Logs contain lengths and SHA-256 only.
- Text input with binary output matches the expected bytes; malformed input is rejected. Conversion to libpq's signed `int` length accepts `INT_MAX` and rejects one-over before narrowing.
- The combined query suite created and cleared 121 results on each architecture.

### Single-row streaming and late errors

`PQsetSingleRowMode` is called immediately after send. Each `PGRES_SINGLE_TUPLE` is consumed and cleared independently, followed by exactly one terminal result and a final null `PQgetResult`. The normal fixture returns 100 ordered rows. The late-error fixture publishes three rows and then exposes `22012`; it is never converted into success or retried.

### Secure cancellation

Only the modern `PQcancelCreate`, `PQcancelStart`, `PQcancelPoll`, `PQcancelStatus`, `PQcancelSocket`, `PQcancelReset`, and `PQcancelFinish` contour is used. Cancellation has an independent deadline and is followed by bounded drain-or-destroy logic.

Cancellation before send, while waiting, after first-row publication, between rows, timeout, and broken-network paths were tested. A clean `57014` drain permits reuse; timeout or damaged protocol state forces destruction. Because the local server can buffer rows faster than the cancel round trip, a cancel triggered after the first row can observe additional already-delivered rows. Production code must therefore preserve published-row accounting and still drain to terminal state rather than assume immediate cessation. The wait case passed ten consecutive iterations per architecture.

### Ownership and concurrency

Eight simultaneous workers each own an independent `PGconn` and their own `PGresult` objects. Normal, close-one, and one-worker-failure cases complete without cross-worker effects. The normal x64 run observed 8,000 rows, 8 connections/finishes, and 8,008 results/clears. A 1,000-cycle offline ownership harness completed with exact cleanup.

The process handle count stabilizes within the prototype threshold after Winsock/OpenSSL process initialization (observed delta 29 in the recorded x64 run). This process-global initialization cost must not be mistaken for per-connection leakage.

### Transactions and connection loss

BEGIN/COMMIT/ROLLBACK, savepoint recovery, an error outside a savepoint, invalid release order (`3B001`), and active-stream conflict expose the observed old/new transaction state. There is no hidden reconnect or retry.

Loss before a terminal command is accepted is classified non-ambiguous; loss after a terminal command is accepted is classified ambiguous. Both cases destroy the connection. An ambiguous commit outcome is never reported as success and is never retried automatically.

### PostGIS discovery

Discovery is anchored on `pg_extension` plus namespace, type, and procedure catalogs and produces schema-qualified calls; an unrelated same-name object is not accepted as a capability signal. Separate absent and PostGIS 3.4.1 present contours passed on Win32 and x64. The present contour used the default extension schema and verified catalog-derived geometry/geography OIDs and required functions, 14 geometry WKT-to-WKB round trips across Point, LineString, Polygon, Multi* and GeometryCollection classes, four EMPTY variants, 2D/Z/M/ZM dimensions, a geography WKT/WKB round trip, NULL-vs-EMPTY, explicit SRID-mismatch rejection without transformation, malformed WKT/WKB errors, terminal drain, and exact result cleanup. A custom extension-schema stand remains unavailable, so that later production contour is not claimed by Stage 0.

### Million-row stream and controlled loss

All three recorded million-row runs returned exactly 1,000,000 ordered rows as 1,000,001 results, with 1,000,001 clears. RSS growth remained bounded and did not scale with the result count.

| Architecture/run | First row | Duration | Baseline RSS | Peak RSS | RSS growth |
|---|---:|---:|---:|---:|---:|
| Win32 / 1 | 32 ms | 362,547 ms | 2,101,248 B | 19,439,616 B | 17,338,368 B |
| x64 / 1 | 47 ms | 369,187 ms | 2,039,808 B | 19,898,368 B | 17,858,560 B |
| x64 / 2 | 47 ms | 371,234 ms | 2,052,096 B | 19,910,656 B | 17,858,560 B |

Injected loss before the first row is classified as eligible for a policy-controlled experiment but is not automatically retried by the prototype. Loss after a row or inside a transaction forbids retry and destroys the connection. Cancellation and terminal-transaction loss are covered by their dedicated prototypes.

## Commands

Supply the six required `VPS_TEST_*` connection values through an ignored local configuration. Set optional `VPS_TEST_POSTGIS_DATABASE` to a separate PostGIS-enabled database for the present contour, then run:

```powershell
pwsh -NoProfile -File scripts/deps/test-versions.ps1
pwsh -NoProfile -File scripts/deps/verify-static-deps.ps1 -TestWrongArchitecture -TestWrongRuntime
pwsh -NoProfile -File scripts/deps/verify-libpq.ps1 -TestWrongArchitecture -TestMissingFeature
pwsh -NoProfile -File scripts/prototypes/build.ps1 -Clean
pwsh -NoProfile -File scripts/prototypes/test-connect.ps1 -IncludeServerCases
pwsh -NoProfile -File scripts/prototypes/test-stage0.ps1 -Architecture x86 -MillionRuns 1
pwsh -NoProfile -File scripts/prototypes/test-stage0.ps1 -Architecture x64 -MillionRuns 2
pwsh -NoProfile -File scripts/prototypes/test-postgis.ps1
```

The Stage 0 runner rejects unsafe log content, records aggregate manifests, and inspects executable imports. Build products and evidence remain outside Git.
