# VirtualPostgreSQL

> Portable C11 SQLite Virtual Table access to PostgreSQL through static,
> asynchronous libpq.

VirtualPostgreSQL is a portable C11 SQLite Virtual Table extension for PostgreSQL.
The project is under staged implementation. The Windows foundation now includes
the extension ABI, secure connection/session runtime, async client port,
versioned PostgreSQL metadata, the end-to-end read-only virtual table path,
conservative predicate/projection/order/limit pushdown, bounded streaming and
database-scoped cancellation.

The normative requirements are maintained outside the tracked release set in
`VirtualPostgreSQL_Technical_Specification.md`.

## Quick start

Run the current Windows build and test matrix from PowerShell 7:

```powershell
pwsh -NoProfile -File scripts/ci/run-stage9.ps1
```

The gate builds and tests MSVC Win32 Debug, MSVC x64 Release, clang-cl x64 Debug
and clang-cl x64 ASan. Linux and Android are long-term targets whose gates start
only after all Windows 1.0 stages close.

## Verification example

The metadata unit probe emits one architecture-independent vector:

```text
schema_fingerprint_vector version=1 value=f563dd9d...a3b6821b
```

Live catalog contours receive credentials only through process-local environment
variables and do not read table rows. Product TLS policy remains
`sslmode=verify-full`; local no-TLS contours do not change that default.

## Documentation

| Guide | Description |
|---|---|
| [Connection credentials](docs/connection-credentials.md) | Credential modes, TLS and session baseline |
| [Client runtime](docs/client-runtime.md) | Async libpq capabilities and diagnostics |
| [Table metadata](docs/table-metadata.md) | Catalog snapshots, keys and fingerprints |
| [Query sources](docs/query-sources.md) | Bounded query admission and read-only boundary |
| [Read-only virtual table](docs/read-only-vtable.md) | SQLite callbacks, codecs, streaming and row identity |
| [Planner and pushdown](docs/planner-pushdown.md) | Compiled plans, exactness, projection, ordering and cost |
| [Streaming and cancellation](docs/streaming-cancellation.md) | Cursor states, limits, cleanup, cancel API and concurrency gates |

## License

See [LICENSE](LICENSE) and dependency notices in [licenses/README.md](licenses/README.md).
