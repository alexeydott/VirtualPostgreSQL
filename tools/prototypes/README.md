# VirtualPostgreSQL Stage 0 prototypes

This directory contains disposable-but-reproducible libpq programs used to prove Windows contracts before production modules are designed. Prototype code may call `PQ*` directly; production code added in later stages must place all libpq ownership in `vps_libpq_client*`.

## Scope

- Win32/x64 static OpenSSL, zlib and libpq builds from [`deps/versions.json`](../../deps/versions.json).
- Async connect and extended-query polling with monotonic deadlines.
- Immediate single-row mode, terminal result handling and late-error propagation.
- Exact binary `bytea`, secure cancellation and bounded drain-or-destroy.
- Eight independent connections, transaction/savepoint states, PostGIS discovery and controlled network loss.

Stage 0 does not create the production extension, public ABI or connection pool. Build products and runtime evidence stay under ignored `build/`.

## Security boundary

- Pass connection settings only through process environment or an explicitly supplied local test configuration outside Git.
- Never place passwords, connection strings, raw SQL, bound values, rows or spatial payloads in source, command lines, logs or evidence.
- The local no-SSL stand is valid only with explicit `sslmode=disable`; it does not satisfy TLS `verify-full` gates.
- Prototype logs contain operation/state, duration, counts, OIDs, SQLSTATE, safe hashes and redacted connection fingerprints only.

The runtime reads `VPS_TEST_HOST`, `VPS_TEST_PORT`, `VPS_TEST_USER`, `VPS_TEST_PASSWORD`, `VPS_TEST_DATABASE`, and `VPS_TEST_SSLMODE`. `VPS_TEST_POSTGIS_DATABASE` selects a separate PostGIS-enabled database for the optional present-contour matrix. Keep all values in an ignored local configuration and export them only into the test process environment.

## Reproducibility entry point

```powershell
pwsh -NoProfile -File scripts/deps/versions.ps1 -RequireArchives
pwsh -NoProfile -File scripts/deps/test-versions.ps1
```

Dependency build and prototype commands are added task-by-task. All commands use out-of-source architecture-specific roots under `build/`.

Build and verify the pinned static libpq contour:

```powershell
pwsh -NoProfile -File scripts/deps/build-libpq.ps1 -Clean
pwsh -NoProfile -File scripts/deps/verify-libpq.ps1 -TestWrongArchitecture -TestMissingFeature
```

Build and run the Stage 0 prototype matrix after supplying the six `VPS_TEST_*` environment values from a local ignored configuration:

```powershell
pwsh -NoProfile -File scripts/prototypes/build.ps1 -Clean
pwsh -NoProfile -File scripts/prototypes/test-connect.ps1 -IncludeServerCases
pwsh -NoProfile -File scripts/prototypes/test-stage0.ps1 -Architecture x86 -MillionRuns 1
pwsh -NoProfile -File scripts/prototypes/test-stage0.ps1 -Architecture x64 -MillionRuns 2
pwsh -NoProfile -File scripts/prototypes/test-postgis.ps1
```

`test-stage0.ps1` writes only redacted aggregate evidence under ignored `build/prototype-evidence/`, rejects credential/connection/SQL text in logs, and inspects every executable for external libpq/OpenSSL/zlib DLL imports.
