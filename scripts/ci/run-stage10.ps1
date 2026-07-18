[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [switch]$RunLocalStand
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'stage10-materialization'
$rootPath = (Resolve-Path -LiteralPath $Root).Path
if ($env:OS -ne 'Windows_NT') { throw 'stage10 requires Windows' }
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

$release = Join-Path $rootPath 'build\stage1-msvc-x64-release'
$vcvars = Join-Path $VisualStudioRoot 'VC\Auxiliary\Build\vcvars64.bat'
$dll = Join-Path $release 'virtualpostgresql.dll'
$privateLibrary = Join-Path $release 'vps_embedded_sqlite.lib'
$exports = & cmd.exe /d /s /c "call `"$vcvars`" >nul && dumpbin /exports `"$dll`""
if ($LASTEXITCODE -ne 0) { throw 'DLL export inspection failed' }
$genericExports = @($exports | Select-String -Pattern '\bsqlite3_' |
    Where-Object { $_.Line -notmatch '\bsqlite3_virtualpostgresql_init\b' })
if ($genericExports.Count -ne 0) { throw 'generic sqlite3 export detected' }
$symbols = & cmd.exe /d /s /c "call `"$vcvars`" >nul && dumpbin /symbols `"$privateLibrary`""
if ($LASTEXITCODE -ne 0) { throw 'private SQLite symbol inspection failed' }
if (@($symbols | Select-String -Pattern 'External\s+\|\s+_?sqlite3_').Count -ne 0) {
    throw 'external generic sqlite3 symbol detected in private target'
}

& (Join-Path $release 'vps_materialization_performance_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'materialization performance gate failed' }

if ($RunLocalStand) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-vtable-stand.ps1') `
        -Root $rootPath -BuildDirectory 'build/stage1-msvc-x64-release'
    if ($LASTEXITCODE -ne 0) { throw 'local materialization stand failed' }
}

$after = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
if (@(Compare-Object -ReferenceObject $before -DifferenceObject $after).Count -ne 0) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed `
        -FailureClass 'source_tree_mutated'
    exit 1
}
$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "duration_ms=$($started.ElapsedMilliseconds),presets=4,local=$($RunLocalStand.IsPresent),exports=isolated,samples=3,probes=100"
