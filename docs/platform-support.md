[← Previous: Troubleshooting](troubleshooting.md) · [Back to README](../README.md) · [Next: Release notes →](release-notes-current.md)

# Supported platforms

## Currently supported Windows runtime

| Contour | Status |
|---|---|
| Windows 10/11 x86 and x64 | supported |
| Windows Server 2019/2022+ x86-compatible/x64 | supported for the matching architecture |
| SQLite host 3.44.0+ | required; module v4 and `xIntegrity` |
| PostgreSQL 15–18 | mandatory tested matrix |
| PostgreSQL 14 | optional legacy contour, not mandatory |
| PostGIS | optional catalog-discovered capability |

The package contains separate Win32 and x64 DLLs. Runtime dependencies are
limited to approved Windows system DLLs; libpq, OpenSSL, zlib, and private
SQLite are linked statically.

The Windows Credential Manager adapter is available through the exported
`virtualpostgresql_wincred_provider` factory and may be installed on a loaded
SQLite connection with `virtualpostgresql_register_credential_provider`.
Implementation-only `vps_wincred_*` symbols remain hidden.

## Long-term contours

Currently, Linux and Android remain long-term development tracks and are not
part of the supported runtime. The presence of portable adapters does not imply
build, runtime, or packaging support.

Stock Android SQLite is normally built without loadable-extension support. A
future Android package requires an application-controlled SQLite host with
dynamic loading enabled or a static-registration entry point. Building an
`.so` alone does not prove Android compatibility; runtime tests on
arm64-v8a/x86_64 are mandatory.

## See Also

- [Building](building.md) — Windows toolchain and presets.
- [Security](security.md) — platform credential and temporary-file requirements.
- [Client runtime](client-runtime.md) — static libpq capabilities.
