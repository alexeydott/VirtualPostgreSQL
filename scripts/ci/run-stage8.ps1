[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [switch]$RunLocalStand
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'stage8-windows'
$rootPath = (Resolve-Path -LiteralPath $Root).Path
if ($env:OS -ne 'Windows_NT') { throw '[stage8-windows] Windows host required' }
$before = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw '[stage8-windows] unable to read source-tree state' }
$started = [Diagnostics.Stopwatch]::StartNew()

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-source-tree.ps1') -Root $rootPath
if ($LASTEXITCODE -ne 0) { throw '[stage8-windows] source-tree gate failed' }

$plannerSources = @(
    (Join-Path $rootPath 'src\vps_planner.c')
    (Join-Path $rootPath 'src\vps_module.c')
)
if (Select-String -LiteralPath $plannerSources -Pattern '\bEXPLAIN\b' -Quiet) {
    throw '[stage8-windows] remote EXPLAIN is forbidden in production planner sources'
}

foreach ($preset in @('msvc-x86-debug', 'msvc-x64-release', 'clang-cl-x64-debug', 'clang-cl-x64-asan')) {
    & pwsh -NoProfile -File (Join-Path $rootPath 'scripts\build-stage1.ps1') `
        -Preset $preset -VisualStudioRoot $VisualStudioRoot
    if ($LASTEXITCODE -ne 0) { throw "[stage8-windows] build preset failed: $preset" }
}

if ($RunLocalStand) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-vtable-stand.ps1') `
        -Root $rootPath -BuildDirectory 'build/stage1-msvc-x64-release'
    if ($LASTEXITCODE -ne 0) { throw '[stage8-windows] planner stand failed' }
}

$after = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw '[stage8-windows] unable to re-read source-tree state' }
if (@(Compare-Object -ReferenceObject $before -DifferenceObject $after).Count -ne 0) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed `
        -FailureClass 'source_tree_mutated'
    exit 1
}
$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "duration_ms=$($started.ElapsedMilliseconds),presets=4,local=$($RunLocalStand.IsPresent),planner_fuzz_smoke=100000"
