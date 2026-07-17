[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$VisualStudioRoot = 'D:\Visual Studio2022'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'stage1-windows'
$rootPath = (Resolve-Path -LiteralPath $Root).Path
if ($env:OS -ne 'Windows_NT') {
    throw '[stage1-windows] Windows host required'
}

$before = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw '[stage1-windows] unable to read source-tree state' }
$started = [Diagnostics.Stopwatch]::StartNew()

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-source-tree.ps1') -Root $rootPath
if ($LASTEXITCODE -ne 0) { throw '[stage1-windows] source-tree gate failed' }

foreach ($preset in @('msvc-x86-debug', 'msvc-x64-release', 'clang-cl-x64-debug')) {
    & pwsh -NoProfile -File (Join-Path $rootPath 'scripts\build-stage1.ps1') -Preset $preset -VisualStudioRoot $VisualStudioRoot
    if ($LASTEXITCODE -ne 0) { throw "[stage1-windows] build preset failed: $preset" }
}

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-windows-binary.ps1') -DllPath (Join-Path $rootPath 'build\stage1-msvc-x86-debug\virtualpostgresql.dll') -Architecture x86 -VisualStudioRoot $VisualStudioRoot
if ($LASTEXITCODE -ne 0) { throw '[stage1-windows] x86 binary inspection failed' }
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-windows-binary.ps1') -DllPath (Join-Path $rootPath 'build\stage1-msvc-x64-release\virtualpostgresql.dll') -Architecture x64 -VisualStudioRoot $VisualStudioRoot
if ($LASTEXITCODE -ne 0) { throw '[stage1-windows] x64 binary inspection failed' }
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-windows-binary.ps1') -DllPath (Join-Path $rootPath 'build\stage1-clang-cl-x64-debug\virtualpostgresql.dll') -Architecture x64 -VisualStudioRoot $VisualStudioRoot
if ($LASTEXITCODE -ne 0) { throw '[stage1-windows] clang-cl binary inspection failed' }

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-negative-fixtures.ps1') -Root $rootPath -VisualStudioRoot $VisualStudioRoot
if ($LASTEXITCODE -ne 0) { throw '[stage1-windows] negative fixtures failed' }

$after = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw '[stage1-windows] unable to re-read source-tree state' }
if (@(Compare-Object -ReferenceObject $before -DifferenceObject $after).Count -ne 0) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'source_tree_mutated'
    exit 1
}

$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed -Detail "duration_ms=$($started.ElapsedMilliseconds),presets=3"
