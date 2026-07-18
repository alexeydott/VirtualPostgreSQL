[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/stage1-msvc-x64-release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$requiredEnvironment = @(
    'VPS_SESSION_TEST_HOST',
    'VPS_SESSION_TEST_PORT',
    'VPS_SESSION_TEST_USER',
    'VPS_SESSION_TEST_PASSWORD',
    'VPS_SESSION_TEST_DBNAME'
)
foreach ($name in $requiredEnvironment) {
    if (-not (Test-Path "Env:$name")) {
        throw "[session-stand] required runtime environment variable is missing: $name"
    }
}

& (Join-Path $PSScriptRoot 'test-async-connect-stand.ps1') `
    -Contour Local -BuildDirectory $BuildDirectory
if ($LASTEXITCODE -ne 0) {
    throw '[session-stand] session baseline contour failed'
}
Write-Output '[session-stand] status=passed transport=async user_data_queries=0'
