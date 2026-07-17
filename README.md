# VirtualPostgreSQL

VirtualPostgreSQL is a portable C11 SQLite Virtual Table extension for PostgreSQL.
The project is currently under staged implementation; Stage 1 establishes the
repository, ABI, platform, build, and loadable-extension skeleton.

The normative product requirements are maintained outside the tracked release
set in `VirtualPostgreSQL_Technical_Specification.md`. Build and usage guidance
will be added as the corresponding implementation stages close.

## Build and verification

Stage 1 provides the portable ABI/platform/extension skeleton and a Windows
MSVC/clang-cl matrix. Linux and Android are long-term targets; their build and
runtime contours start only after every Windows 1.0 stage is closed.

Run the complete provider-neutral Stage 1 gate from a PowerShell 7 prompt:

```powershell
pwsh -NoProfile -File scripts/ci/run-stage1.ps1
```

The gate configures, builds and tests MSVC Win32 Debug, MSVC x64 Release and
clang-cl x64 Debug, then inspects DLL architecture, exports, imports and version
resources. It also checks the release set, dependency license metadata, flat
`src/` layout and platform-header isolation. Deliberate negative fixtures live
only under ignored `build/` and must leave the source-tree state unchanged.
Set `VPS_CI_LOG_LEVEL` to `debug`, `info`, `warn`, `error` or `off` to control
structured gate logging; the default is `info`.

Product TLS policy remains `sslmode=verify-full`; local no-TLS test contours do
not change that default.
