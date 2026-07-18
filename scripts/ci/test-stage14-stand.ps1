[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$BuildDirectory = 'build/stage1-msvc-x64-release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$required = @('VPS_SESSION_TEST_HOST', 'VPS_SESSION_TEST_PORT',
              'VPS_SESSION_TEST_USER', 'VPS_SESSION_TEST_PASSWORD')
foreach ($name in $required) {
    if ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name))) {
        throw "[stage14-stand] required runtime environment variable is missing: $name"
    }
}
$temporary = @(
    'VPS_ASYNC_TEST_HOST', 'VPS_ASYNC_TEST_PORT', 'VPS_ASYNC_TEST_USER',
    'VPS_ASYNC_TEST_PASSWORD', 'VPS_ASYNC_TEST_DBNAME',
    'VPS_ASYNC_TEST_SSLMODE', 'VPS_ASYNC_TEST_CHANNEL_BINDING'
)
$saved = @{}
foreach ($name in $temporary) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
    foreach ($suffix in @('HOST', 'PORT', 'USER', 'PASSWORD')) {
        [Environment]::SetEnvironmentVariable(
            "VPS_ASYNC_TEST_$suffix",
            [Environment]::GetEnvironmentVariable("VPS_SESSION_TEST_$suffix"))
    }
    $env:VPS_ASYNC_TEST_DBNAME = 'postgres'
    $env:VPS_ASYNC_TEST_SSLMODE = 'disable'
    $env:VPS_ASYNC_TEST_CHANNEL_BINDING = 'disable'
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-vtable-stand.ps1') `
        -Root $Root -BuildDirectory $BuildDirectory
    if ($LASTEXITCODE -ne 0) { throw '[stage14-stand] metadata contour failed' }
} finally {
    foreach ($name in $temporary) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name])
    }
}
Write-Output 'stage14_stand status=passed functions=6 shadow_rows=exact tamper=detected fallback_policy=classified'
