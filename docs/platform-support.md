[← Previous: Troubleshooting](troubleshooting.md) · [Back to README](../README.md) · [Next: Release notes →](release-notes-1.0.0.md)

# Поддерживаемые платформы

## Windows 1.0

| Контур | Статус |
|---|---|
| Windows 10/11 x86 и x64 | supported |
| Windows Server 2019/2022+ x86-compatible/x64 | supported по соответствующей architecture |
| SQLite host 3.44.0+ | required; module v4 и `xIntegrity` |
| PostgreSQL 15–18 | mandatory tested matrix |
| PostgreSQL 14 | optional legacy, не mandatory |
| PostGIS | optional catalog-discovered capability |

Package содержит отдельные Win32 и x64 DLL. Runtime dependencies должны быть
только approved Windows system DLL; libpq/OpenSSL/zlib/private SQLite статичны.

## Долгосрочные contours

Linux и Android выполняются только после закрытия Windows 1.0 и не входят в
эту release claim. Наличие portable adapters не означает runtime support.

Stock Android SQLite обычно собран без loadable extensions. Будущий Android
package потребует application-controlled SQLite host с разрешённой dynamic
load либо static registration entry point. Компиляция `.so` сама по себе не
доказывает Android compatibility; runtime tests на arm64-v8a/x86_64 обязательны.

## See Also

- [Building](building.md) — Windows toolchain и presets.
- [Security](security.md) — platform credential/temp requirements.
- [Client runtime](client-runtime.md) — static libpq capabilities.
