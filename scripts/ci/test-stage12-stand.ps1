[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$BuildDirectory = 'build/stage1-msvc-x64-release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$required = @('VPS_SESSION_TEST_HOST', 'VPS_SESSION_TEST_PORT',
              'VPS_SESSION_TEST_USER', 'VPS_SESSION_TEST_PASSWORD',
              'VPS_SESSION_TEST_DBNAME')
foreach ($name in $required) {
    if ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name))) {
        throw "[stage12-stand] required runtime environment variable is missing: $name"
    }
}

$saved = @{}
$temporary = @(
    'VPS_SESSION_TEST_FIXTURE', 'VPS_ASYNC_TEST_HOST', 'VPS_ASYNC_TEST_PORT',
    'VPS_ASYNC_TEST_USER', 'VPS_ASYNC_TEST_PASSWORD', 'VPS_ASYNC_TEST_DBNAME',
    'VPS_ASYNC_TEST_SSLMODE', 'VPS_ASYNC_TEST_CHANNEL_BINDING',
    'VPS_VTAB_TEST_TRANSACTIONS'
)
foreach ($name in $temporary) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
    $env:VPS_SESSION_TEST_FIXTURE = '1'
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-async-connect-stand.ps1') `
        -Contour Local -BuildDirectory $BuildDirectory
    if ($LASTEXITCODE -ne 0) { throw '[stage12-stand] fixture bootstrap failed' }
    foreach ($suffix in @('HOST', 'PORT', 'USER', 'PASSWORD', 'DBNAME')) {
        [Environment]::SetEnvironmentVariable(
            "VPS_ASYNC_TEST_$suffix",
            [Environment]::GetEnvironmentVariable("VPS_SESSION_TEST_$suffix"))
    }
    $env:VPS_ASYNC_TEST_SSLMODE = 'disable'
    $env:VPS_ASYNC_TEST_CHANNEL_BINDING = 'disable'
    $env:VPS_VTAB_TEST_TRANSACTIONS = '1'
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-vtable-stand.ps1') `
        -Root $Root -BuildDirectory $BuildDirectory
    if ($LASTEXITCODE -ne 0) {
        throw '[stage12-stand] transaction contour failed'
    }
} finally {
    foreach ($name in $temporary) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name])
    }
}
Write-Output 'stage12_stand status=passed operations=begin,sync,commit,rollback,savepoint,rollback-to,release recovery=aborted,busy'
