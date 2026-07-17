[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\..\build\stage1-msvc-x64-release\vps_support_integration_test.exe')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'secure-zero-optimized'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$started = [Diagnostics.Stopwatch]::StartNew()

& $executablePath --optimized-zero-only
if ($LASTEXITCODE -ne 0) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'runtime_observation'
    exit 1
}

$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed -Detail "duration_ms=$($started.ElapsedMilliseconds),configuration=release"
