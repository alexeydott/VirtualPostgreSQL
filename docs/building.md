[Back to README](../README.md) · [Next: Connection and credentials →](connection-credentials.md)

# Building Windows 1.0

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
configured; the mandatory release gate starts pinned local PostgreSQL 15–18
instances itself.

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

The complete release workflow—two reproducible Win32/x64 builds, PE and package
inspection, SBOM and provenance generation, and Windows 1.0 acceptance—runs
with one command:

```powershell
pwsh -NoProfile -File scripts/package-windows.ps1
```

The verified archive is published as
`dist/VirtualPostgreSQL-1.0.0-windows.zip`. Git permits only versioned
`VirtualPostgreSQL-*.zip` files in `dist/`; intermediate staging directories
and every other `dist/` file remain ignored.

## See Also

- [Static analysis](static-analysis.md) — three independent analyzer contours.
- [Sanitizers](sanitizers.md) — supported clang-cl configurations.
- [Platform support](platform-support.md) — exact Windows 1.0 matrix.
