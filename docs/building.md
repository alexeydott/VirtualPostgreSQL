[Back to README](../README.md) · [Next: Connection and credentials →](connection-credentials.md)

# Building on Windows

## Requirements

| Component | Supported option |
|---|---|
| OS | Windows 10/11 or Windows Server 2019/2022+ |
| Compiler | Visual Studio 2022 MSVC, x86 and x64 |
| Build tools | CMake 3.25+, Ninja, PowerShell 7 |
| Additional gates | clang-cl 19+, PVS-Studio 7.29 |

Toolchain paths can be supplied through script parameters. The libpq, OpenSSL,
zlib, and private SQLite dependencies are pinned by build manifests and linked
statically; the release DLL must not require their DLLs.

## Regular build

```powershell
pwsh -NoProfile -File scripts/build-stage1.ps1 -Preset msvc-x86-debug
pwsh -NoProfile -File scripts/build-stage1.ps1 -Preset msvc-x64-release
```

The resulting loadable extension is placed in the corresponding
`build/<preset>/` directory. The script configures CMake, builds all targets,
and runs CTest. Network tests report `skipped` when no runtime fixture is
configured. The separate `run-pg-matrix.ps1` gate starts the provisioned local
PostgreSQL 15–18 instances and tests both client architectures.

## Quality gates

```powershell
pwsh -NoProfile -File scripts/ci/run-static-analysis.ps1 -Architecture All
pwsh -NoProfile -File scripts/ci/run-sanitizers.ps1
pwsh -NoProfile -File scripts/ci/run-fuzz.ps1
pwsh -NoProfile -File scripts/ci/run-pg-matrix.ps1
pwsh -NoProfile -File scripts/ci/run-hardening.ps1
pwsh -NoProfile -File scripts/ci/run-performance.ps1
```

Generated build and evidence trees are ignored by Git and are not included in
the package. Release binaries are produced by the separate clean
reproducibility workflow recorded in the release manifest.

## Release package and dist

The packaging workflow performs two reproducible Win32/x64 builds, PE and
package inspection, SBOM and provenance generation, documentation/source-tree
checks, and current acceptance-matrix validation:

```powershell
pwsh -NoProfile -File scripts/package-windows.ps1
```

The heavyweight static-analysis, sanitizer, fuzz, PostgreSQL matrix, hardening,
and performance gates are separate prerequisites; packaging does not rerun
them. The verified archive is published as
`dist/VirtualPostgreSQL-*-windows.zip`. Git permits only matching release ZIP
files in `dist/`; intermediate staging directories and every other `dist/`
file remain ignored.

## See Also

- [Static analysis](static-analysis.md) — three independent analyzer contours.
- [Sanitizers](sanitizers.md) — supported clang-cl configurations.
- [Platform support](platform-support.md) — current Windows matrix.
