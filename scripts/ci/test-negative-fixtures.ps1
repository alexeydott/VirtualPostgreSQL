[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$ReleaseDll = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'build\stage1-msvc-x64-release\virtualpostgresql.dll'),
    [string]$VisualStudioRoot = 'D:\Visual Studio2022'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'negative-fixtures'
$rootPath = Resolve-Path -LiteralPath $Root
$fixtureRoot = Join-Path $rootPath.Path "build\ci-fixtures\$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null

function Invoke-ExpectedFailure {
    param([string]$Name, [string]$Script, [string[]]$Arguments)
    & pwsh -NoProfile -File $Script @Arguments *> $null
    if ($LASTEXITCODE -eq 0) {
        throw "[negative-fixtures] fixture unexpectedly passed: $Name"
    }
    Write-VpsCiEvent -Gate $gate -Level info -Status passed -FailureClass $Name
}

try {
    $platform = Join-Path $fixtureRoot 'platform'
    New-Item -ItemType Directory -Path (Join-Path $platform 'src') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $platform 'src\portable.c') -Value '#include <windows.h>' -Encoding utf8NoBOM
    Invoke-ExpectedFailure -Name 'platform_header_leak' -Script (Join-Path $PSScriptRoot 'test-source-tree.ps1') -Arguments @('-Root', $platform, '-Check', 'PlatformHeaders')

    $flat = Join-Path $fixtureRoot 'flat'
    New-Item -ItemType Directory -Path (Join-Path $flat 'src\nested') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $flat 'src\nested\unit.c') -Value 'int fixture;' -Encoding utf8NoBOM
    Invoke-ExpectedFailure -Name 'src_subdirectory' -Script (Join-Path $PSScriptRoot 'test-source-tree.ps1') -Arguments @('-Root', $flat, '-Check', 'FlatSrc')

    $generated = Join-Path $fixtureRoot 'generated'
    New-Item -ItemType Directory -Path $generated -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $generated 'unexpected.obj') -Value 'fixture' -Encoding utf8NoBOM
    Invoke-ExpectedFailure -Name 'generated_file' -Script (Join-Path $PSScriptRoot 'test-source-tree.ps1') -Arguments @('-Root', $generated, '-Check', 'ForbiddenFiles')

    $license = Join-Path $fixtureRoot 'license'
    New-Item -ItemType Directory -Path (Join-Path $license 'deps') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $license 'licenses') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $license 'LICENSE') -Value 'fixture' -Encoding utf8NoBOM
    Set-Content -LiteralPath (Join-Path $license 'licenses\README.md') -Value 'fixture' -Encoding utf8NoBOM
    Set-Content -LiteralPath (Join-Path $license 'deps\versions.json') -Value '{"dependencies":[{"name":"fixture"}]}' -Encoding utf8NoBOM
    Invoke-ExpectedFailure -Name 'missing_license' -Script (Join-Path $PSScriptRoot 'test-source-tree.ps1') -Arguments @('-Root', $license, '-Check', 'Licenses')

    if (-not (Test-Path -LiteralPath $ReleaseDll -PathType Leaf)) {
        throw '[negative-fixtures] release DLL is missing'
    }
    $shortExports = Join-Path $fixtureRoot 'expected-exports.txt'
    Get-Content -LiteralPath (Join-Path $PSScriptRoot 'expected-exports.txt') | Select-Object -SkipLast 1 | Set-Content -LiteralPath $shortExports -Encoding utf8NoBOM
    Invoke-ExpectedFailure -Name 'unexpected_export' -Script (Join-Path $PSScriptRoot 'test-windows-binary.ps1') -Arguments @('-DllPath', $ReleaseDll, '-Architecture', 'x64', '-VisualStudioRoot', $VisualStudioRoot, '-ExpectedExportsPath', $shortExports, '-SkipImportCheck')

    $emptyImports = Join-Path $fixtureRoot 'allowed-imports.txt'
    Set-Content -LiteralPath $emptyImports -Value '# no imports allowed' -Encoding utf8NoBOM
    Invoke-ExpectedFailure -Name 'unexpected_import' -Script (Join-Path $PSScriptRoot 'test-windows-binary.ps1') -Arguments @('-DllPath', $ReleaseDll, '-Architecture', 'x64', '-VisualStudioRoot', $VisualStudioRoot, '-AllowedImportsPath', $emptyImports)
} finally {
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-VpsCiEvent -Gate $gate -Level info -Status passed -Detail 'fixtures=6'
