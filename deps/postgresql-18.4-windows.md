# PostgreSQL 18.4 static libpq build notes

The prototype dependency workflow builds only the pinned PostgreSQL client archives required by the test programs. The pinned PostgreSQL source no longer ships the legacy `src/tools/msvc` build; the supported source-tree contour here is Meson + Ninja in an MSVC developer environment.

## Configuration

- `--default-library=static` and only the explicitly selected static targets are compiled.
- `--auto-features=disabled`, `-Dnls=disabled`, `-Dzlib=disabled`, `-Dssl=openssl`.
- OpenSSL include/library roots point at the matching Task 2 architecture and CRT contour; system or preinstalled OpenSSL is not accepted.
- Release uses `-Db_vscrt=mt`; Debug uses `-Db_vscrt=mtd`.
- Flex/Bison are invoked by absolute path from the pinned parser-tool root. Tcl is not used (`pltcl=disabled`).
- Static OpenSSL configure checks require `ws2_32.lib` and `crypt32.lib` in `LDFLAGS`.

The selected feature set keeps UTF-8, SCRAM authentication, asynchronous connect/query APIs, single-row mode and PostgreSQL 18 secure-cancel APIs. NLS, LDAP, GSSAPI, libcurl/OAuth flow, server programs and procedural languages are outside this prototype contour. zlib remains a separately pinned static dependency and is linked by the combined probe, but it is not a direct libpq dependency in this configuration.

## Windows support archives

Meson's `libpq:static_library` archive contains libpq objects but deliberately leaves frontend common/port code as private link dependencies. On this Windows/MSVC contour, `libpgcommon.a` renames encoding symbols to private variants while libpq references the public variants. The verified static consumer set therefore packages the public-symbol support variants produced by `libpgcommon_shlib:static_library` and `libpgport_shlib:static_library` as `pgcommon.lib` and `pgport.lib`. They remain static COFF archives; no PostgreSQL DLL is built or shipped.

Consumer link order:

1. `libpq.lib`, `pgcommon.lib`, `pgport.lib`;
2. `libssl.lib`, `libcrypto.lib` and optionally the co-packaged `zlib.lib`;
3. Windows system libraries `ws2_32`, `secur32`, `crypt32`, `advapi32`, `user32`, `shell32`.

The build is intentionally not patched in place. `scripts/deps/build-libpq.ps1` records the exact Meson options and artifact hashes in ignored per-contour manifests; `scripts/deps/verify-libpq.ps1` is the executable linkage and feature gate.
