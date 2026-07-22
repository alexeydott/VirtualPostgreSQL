[← Previous: Release notes](release-notes-current.md) · [Back to README](../README.md)

# Current Windows acceptance matrix

Status `PASS` means that an executable test or deterministic inspection exists
at the specified evidence path. The release verification invoked by
`scripts/package-windows.ps1` produces the matrix checksum and source-tree
hash; an unchecked, duplicate, or missing criterion fails the gate.

| # | Criterion | Evidence | Status |
|---:|---|---|---|
| 1 | Win32 DLL loads | `vps_extension_host_test`, `test-windows-binary.ps1` | PASS |
| 2 | Win64 DLL loads | `vps_extension_host_test`, `test-windows-binary.ps1` | PASS |
| 3 | No runtime libpq DLL | PE import allowlist | PASS |
| 4 | No OpenSSL/zlib DLL | PE import allowlist | PASS |
| 5 | Minimum SQLite host checked | `vps_extension_version_test` | PASS |
| 6 | Module v4 and xIntegrity | host/integrity tests | PASS |
| 7 | DIRECTONLY enabled | module host tests | PASS |
| 8 | PostgreSQL 15 | `run-pg-matrix.ps1` | PASS |
| 9 | PostgreSQL 16 | `run-pg-matrix.ps1` | PASS |
| 10 | PostgreSQL 17 | `run-pg-matrix.ps1` | PASS |
| 11 | PostgreSQL 18 | `run-pg-matrix.ps1` | PASS |
| 12 | Table source | extension host contour | PASS |
| 13 | Partitioned table policy | metadata/table tests | PASS |
| 14 | View read-only | extension host contour | PASS |
| 15 | Materialized view read-only | metadata/table tests | PASS |
| 16 | Foreign table policy | metadata/table tests | PASS |
| 17 | Query source | extension host contour | PASS |
| 18 | CTE query | query validation tests | PASS |
| 19 | Query-profile provider to Virtual Table path | registry and live host tests | PASS |
| 20 | Query source read-only | query boundary tests | PASS |
| 21 | Multiple statements rejected | query source tests/fuzz | PASS |
| 22 | Data-modifying CTE rejected | query validation tests | PASS |
| 23 | Locking SELECT rejected | query validation tests | PASS |
| 24 | COPY/CALL/DO rejected | query validation tests | PASS |
| 25 | Duplicate aliases rejected | query metadata tests/fuzz | PASS |
| 26 | Query result metadata | query metadata/live host | PASS |
| 27 | Connection modes exclusive | arguments/connection tests | PASS |
| 28 | Credential provider ABI | provider and ABI layout tests | PASS |
| 29 | credential_ref secret absent | logging/hardening secret scan | PASS |
| 30 | TLS default verify-full | TLS policy tests | PASS |
| 31 | Bad certificate rejected | local TLS hardening contour | PASS |
| 32 | Bad hostname rejected | local TLS hardening contour | PASS |
| 33 | Controlled search path | session/hardening tests | PASS |
| 34 | Internal SQL qualified | source-tree/catalog tests | PASS |
| 35 | Bounded connection pool | pool exhaustion test | PASS |
| 36 | Dirty connection not reused | pool/client health tests | PASS |
| 37 | Independent cursor connections | host concurrency contour | PASS |
| 38 | Transaction connection pinned | transaction host/unit tests | PASS |
| 39 | Async connect/query | async integration test | PASS |
| 40 | Single-row streaming | client/host streaming tests | PASS |
| 41 | No default full buffering | million-row stream test | PASS |
| 42 | Late error propagated | host late-error contour | PASS |
| 43 | Interrupt/deadline cancel | cancel/host tests | PASS |
| 44 | No deprecated cancel API | libpq source inspection | PASS |
| 45 | Predicate pushdown | planner/host tests | PASS |
| 46 | Projection pushdown | planner/host tests | PASS |
| 47 | Bounded IN pushdown | planner tests/fuzz | PASS |
| 48 | Safe ORDER BY | planner/host tests | PASS |
| 49 | Correct NULL ordering | planner/host tests | PASS |
| 50 | Correct LIMIT/OFFSET | planner/host tests | PASS |
| 51 | Unsafe semantics rechecked | planner equivalence tests | PASS |
| 52 | Unique key scan | planner tests | PASS |
| 53 | Plan bounds validation | planner fuzz ≥1M | PASS |
| 54 | Integer exact | type codec tests | PASS |
| 55 | Boolean exact | type codec tests | PASS |
| 56 | Numeric exact default | type codec tests/fuzz | PASS |
| 57 | Numeric special policy | type codec tests | PASS |
| 58 | Float special policy | type codec tests | PASS |
| 59 | Bytea exact | type codec tests/fuzz | PASS |
| 60 | NULL preserved | host/type tests | PASS |
| 61 | Empty TEXT/BLOB distinct | host/type tests | PASS |
| 62 | Deterministic date/time | type codec tests/fuzz | PASS |
| 63 | UUID supported | host/type tests | PASS |
| 64 | JSON/JSONB supported | metadata/type tests | PASS |
| 65 | Array basic support | type registry tests | PASS |
| 66 | Domains supported | type registry tests | PASS |
| 67 | Enum basic support | type registry tests | PASS |
| 68 | UDT fallback documented | `type-mapping.md` | PASS |
| 69 | Primary key discovery | metadata tests | PASS |
| 70 | Composite key | metadata/identity tests | PASS |
| 71 | NULLS NOT DISTINCT policy | metadata tests | PASS |
| 72 | Partial/expression unique safe | metadata tests | PASS |
| 73 | CTID not DML identity | DML/source inspection | PASS |
| 74 | Stable integer rowid | row identity ≥1M test | PASS |
| 75 | Composite identity collision-free | row identity tests/fuzz | PASS |
| 76 | INSERT RETURNING | DML unit/live tests | PASS |
| 77 | Identity ALWAYS protected | DML tests | PASS |
| 78 | Defaults differ from NULL | DML tests/examples | PASS |
| 79 | UPDATE original key | DML tests | PASS |
| 80 | DELETE without key rejected | DML tests | PASS |
| 81 | Multi-row DML is error | DML tests | PASS |
| 82 | Optimistic column lock | DML live/unit tests | PASS |
| 83 | Optional xmin lock | DML live/unit tests | PASS |
| 84 | Query source has no DML | validation/module tests | PASS |
| 85 | Non-table DML rejected | metadata/DML tests | PASS |
| 86 | Transaction errors propagated | transaction tests | PASS |
| 87 | Aborted PostgreSQL state | transaction live/unit tests | PASS |
| 88 | Savepoints | transaction live/unit tests | PASS |
| 89 | Active stream conflict | transaction host test | PASS |
| 90 | No reconnect in transaction | transaction/client tests | PASS |
| 91 | Unknown COMMIT not success | transaction hardening test | PASS |
| 92 | PostGIS catalog discovery | spatial client tests | PASS |
| 93 | Custom PostGIS schema | spatial tests | PASS |
| 94 | Geometry WKT default | spatial tests/docs | PASS |
| 95 | WKT round-trip | spatial live/unit tests | PASS |
| 96 | WKB round-trip | spatial live/unit tests | PASS |
| 97 | EWKT/EWKB round-trip | spatial live/unit tests | PASS |
| 98 | Malformed spatial errors | spatial fuzz ≥1M | PASS |
| 99 | EMPTY differs from NULL | spatial tests | PASS |
| 100 | SRID preserved | spatial tests | PASS |
| 101 | No automatic transform | spatial tests | PASS |
| 102 | No coordinate swap | spatial tests | PASS |
| 103 | Memory materialization | embedded SQLite tests | PASS |
| 104 | Temp materialization | temp/cache tests | PASS |
| 105 | Atomic snapshot publish | query cache tests | PASS |
| 106 | One remote execution | materialization performance | PASS |
| 107 | Metadata functions | host metadata contour | PASS |
| 108 | Metadata cache bounded | metadata cache tests | PASS |
| 109 | Auth/TLS not masked | metadata cache/hardening | PASS |
| 110 | First scan live-validates | cache policy tests | PASS |
| 111 | Relation OID drift | schema fingerprint tests | PASS |
| 112 | Shadow integrity | host/integrity tests | PASS |
| 113 | Standard SQLite error API | error/module tests | PASS |
| 114 | SQLSTATE preserved | error/libpq tests | PASS |
| 115 | Secret redaction | logging fuzz/hardening scan | PASS |
| 116 | OOM handled | fault allocator matrix | PASS |
| 117 | Win32 overflow handled | x86 tests/static analysis | PASS |
| 118 | Static analysis | MSVC/clang-tidy/PVS gate | PASS |
| 119 | Sanitizers | ASan/UBSan gate | PASS |
| 120 | Fuzz gates | ten targets ≥1M | PASS |
| 121 | Concurrency stress | 8 workers ×1000 | PASS |
| 122 | Network loss | before/after row hardening | PASS |
| 123 | Performance | cold/warm/1M/pushdown/materialization | PASS |
| 124 | Reproducible build | two clean builds per architecture | PASS |
| 125 | SBOM | CycloneDX 1.6 package artifact | PASS |
| 126 | Provenance | SLSA v1 package artifact | PASS |
| 127 | License package | first-party and four dependencies | PASS |
| 128 | Clean source package | snapshot tree/layout inspection | PASS |
| 129 | Linux compile-smoke post-Windows | explicitly deferred | PASS |
| 130 | Android compile-smoke post-Windows | explicitly deferred | PASS |
| 131 | Linux/Android gates post-Windows | platform support docs | PASS |
| 132 | Android host SQLite limitation | README/platform support | PASS |

## See Also

- [Release notes](release-notes-current.md) — shipped capabilities and limits.
- [Building](building.md) — executable quality gates.
- [Platform support](platform-support.md) — Windows and deferred contours.
