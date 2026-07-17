[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$VisualStudioRoot = 'D:\Visual Studio2022'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'stage2-windows'
$rootPath = (Resolve-Path -LiteralPath $Root).Path
if ($env:OS -ne 'Windows_NT') {
    throw '[stage2-windows] Windows host required'
}

$before = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw '[stage2-windows] unable to read source-tree state' }
$started = [Diagnostics.Stopwatch]::StartNew()

foreach ($preset in @('msvc-x86-debug', 'msvc-x64-release', 'clang-cl-x64-debug', 'clang-cl-x64-asan')) {
    & pwsh -NoProfile -File (Join-Path $rootPath 'scripts\build-stage1.ps1') -Preset $preset -VisualStudioRoot $VisualStudioRoot
    if ($LASTEXITCODE -ne 0) { throw "[stage2-windows] build preset failed: $preset" }
}

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-secure-zero.ps1') -Executable (Join-Path $rootPath 'build\stage1-msvc-x64-release\vps_support_integration_test.exe')
if ($LASTEXITCODE -ne 0) { throw '[stage2-windows] optimized secure-zero inspection failed' }

$after = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw '[stage2-windows] unable to re-read source-tree state' }
if (@(Compare-Object -ReferenceObject $before -DifferenceObject $after).Count -ne 0) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'source_tree_mutated'
    exit 1
}

$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed -Detail "duration_ms=$($started.ElapsedMilliseconds),presets=4"
