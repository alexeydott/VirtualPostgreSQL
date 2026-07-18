[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [ValidateRange(1, 1000000)][int]$FuzzRuns = 10000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$rootPath = Assert-VpsSafeRoot -Root $Root
$gate = 'sanitizers'
$started = [Diagnostics.Stopwatch]::StartNew()
$presets = @('clang-cl-x64-asan', 'clang-cl-x64-ubsan')
$seedFiles = @(Get-ChildItem -LiteralPath (Join-Path $rootPath 'tests\fuzz') `
    -File | Where-Object { $_.Extension -in @('.txt', '.sql') } | `
    Sort-Object FullName | ForEach-Object { $_.FullName })
if ($seedFiles.Count -eq 0) { throw '[sanitizers] deterministic seeds are missing' }

foreach ($preset in $presets) {
    & pwsh -NoProfile -File (Join-Path $rootPath 'scripts\build-stage1.ps1') `
        -Preset $preset -VisualStudioRoot $VisualStudioRoot
    if ($LASTEXITCODE -ne 0) { throw "[sanitizers] preset failed: $preset" }
    $buildPath = if ($preset -like '*asan') {
        Join-Path $rootPath 'build\stage2-clang-cl-x64-asan'
    } else {
        Join-Path $rootPath 'build\stage15-clang-cl-x64-ubsan'
    }
    foreach ($target in @('vps_query_source_fuzz', 'vps_planner_fuzz')) {
        $executable = Join-Path $buildPath "$target.exe"
        if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
            throw "[sanitizers] fuzz executable missing: $target"
        }
        & $executable @seedFiles "-runs=$FuzzRuns" '-max_len=4096' `
            '-timeout=5' '-print_final_stats=1'
        if ($LASTEXITCODE -ne 0) {
            throw "[sanitizers] fuzz regression failed: preset=$preset target=$target"
        }
        Write-VpsCiEvent -Gate $gate -Level info -Status passed `
            -Detail "preset=$preset,target=$target,runs=$FuzzRuns,seeds=$($seedFiles.Count)"
    }
}
$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "duration_ms=$($started.ElapsedMilliseconds),presets=$($presets.Count),fuzz_targets=2"
