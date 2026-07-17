# Pinned prototype dependencies

Stage 0 builds all network dependencies from pinned upstream source archives. Generated source trees, libraries and manifests live under ignored `build/`; this directory stores only reproducibility inputs.

| Dependency | Pin | Source archive | SHA-256 | License |
|---|---:|---|---|---|
| PostgreSQL/libpq | 18.4 | `postgresql-18.4.tar.bz2` | `81a81ec695fb0c7901407defaa1d2f7973617154cf27ba74e3a7ab8e64436094` | PostgreSQL License |
| OpenSSL LTS | 3.5.7 | `openssl-3.5.7.tar.gz` | `a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8` | Apache-2.0 |
| zlib | 1.3.2 | `zlib-1.3.2.tar.gz` | `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16` | Zlib |

The canonical machine-readable manifest is [`versions.json`](versions.json). URLs are HTTPS upstream release locations; after the first acquisition every build verifies the recorded SHA-256 before extraction.

## Toolchain pin

- Visual Studio 2022 `17.14.37301.10` with MSVC tools `14.44.35207` (`cl` `19.44.35227.0`).
- Windows SDK `10.0.26100.0`.
- CMake `4.0.3`, Ninja `1.12.1`, Meson `1.8.2`.
- PowerShell `7.5.8`, Python `3.14.3`, Perl `5.40.2`, NASM `2.16.01`.
- Flex `2.5.35` and Bison `2.4.2` under `D:\tools\bison` (override with `VPS_PARSER_TOOLS_ROOT`).
- Tcl `8.6.7` under `D:\tools\tcl` for the later SQLite upstream test harness (override with `VPS_TCL_ROOT`); PostgreSQL `pltcl` remains disabled.
- Release uses `/MT`; Debug uses `/MTd`; Win32 and x64 build roots are isolated.

Validate the host and downloaded archives:

```powershell
pwsh -NoProfile -File scripts/deps/versions.ps1 -RequireArchives
pwsh -NoProfile -File scripts/deps/test-versions.ps1
```

The validation scripts print only tool names, versions, architectures, safe repository-local paths and hashes. They never enumerate the process environment or connection configuration.

## Update policy

Changing any pin requires updating `versions.json`, this table, license inputs and the expected hashes in reproducibility evidence. A dependency update is not complete until Win32/x64 static builds and the Stage 0 prototype suite pass again.
