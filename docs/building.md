[Back to README](../README.md) · [Next: Connection and credentials →](connection-credentials.md)

# Сборка Windows 1.0

## Требования

| Компонент | Поддерживаемый вариант |
|---|---|
| ОС | Windows 10/11 или Windows Server 2019/2022+ |
| Компилятор | Visual Studio 2022 MSVC, x86 и x64 |
| Build tools | CMake 3.25+, Ninja, PowerShell 7 |
| Дополнительные gates | clang-cl 19+, PVS-Studio 7.29 |

Пути к toolchain можно передать параметрами скриптов. Зависимости libpq,
OpenSSL, zlib и private SQLite закреплены build-манифестами и линкуются
статически; release DLL не должна требовать их DLL.

## Обычная сборка

```powershell
pwsh -NoProfile -File scripts/build-stage1.ps1 -Preset msvc-x86-debug
pwsh -NoProfile -File scripts/build-stage1.ps1 -Preset msvc-x64-release
```

Готовая loadable extension находится в соответствующем `build/<preset>/`.
Скрипт конфигурирует CMake, собирает все targets и запускает CTest. Network
tests имеют status `skipped`, если runtime fixture не задана; mandatory release
gate сам поднимает закреплённые локальные PostgreSQL 15–18.

## Quality gates

```powershell
pwsh -NoProfile -File scripts/ci/run-static-analysis.ps1 -Architecture All
pwsh -NoProfile -File scripts/ci/run-sanitizers.ps1
pwsh -NoProfile -File scripts/ci/run-fuzz.ps1
pwsh -NoProfile -File scripts/ci/run-pg-matrix.ps1
pwsh -NoProfile -File scripts/ci/run-hardening.ps1
pwsh -NoProfile -File scripts/ci/run-performance.ps1
```

Generated build/evidence trees игнорируются Git и не входят в package.
Release build выполняется отдельным clean reproducibility workflow, описанным
в release manifest.

## See Also

- [Static analysis](static-analysis.md) — три независимых analyzer contour.
- [Sanitizers](sanitizers.md) — поддерживаемые clang-cl конфигурации.
- [Platform support](platform-support.md) — точная матрица Windows 1.0.
