[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [switch]$RunLocalStand
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'stage14-metadata-cache-integrity'
$rootPath = Assert-VpsSafeRoot -Root $Root
$before = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw 'unable to read source-tree state' }
$started = [Diagnostics.Stopwatch]::StartNew()

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-source-tree.ps1') -Root $rootPath
if ($LASTEXITCODE -ne 0) { throw 'source-tree gate failed' }
foreach ($preset in @('msvc-x86-debug', 'msvc-x64-release',
                       'clang-cl-x64-debug', 'clang-cl-x64-asan')) {
    & pwsh -NoProfile -File (Join-Path $rootPath 'scripts\build-stage1.ps1') `
        -Preset $preset -VisualStudioRoot $VisualStudioRoot
    if ($LASTEXITCODE -ne 0) { throw "build preset failed: $preset" }
}
if ($RunLocalStand) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-stage14-stand.ps1') `
        -Root $rootPath
    if ($LASTEXITCODE -ne 0) { throw 'local Stage 14 stand failed' }
}
$after = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
if (@(Compare-Object -ReferenceObject $before -DifferenceObject $after).Count -ne 0) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed `
        -FailureClass 'source_tree_mutated'
    exit 1
}
$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "duration_ms=$($started.ElapsedMilliseconds),presets=4,local=$($RunLocalStand.IsPresent)"
