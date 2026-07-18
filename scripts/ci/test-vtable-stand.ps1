[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$BuildDirectory = 'build/stage1-msvc-x64-release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$required = @(
    'VPS_ASYNC_TEST_HOST',
    'VPS_ASYNC_TEST_PORT',
    'VPS_ASYNC_TEST_USER',
    'VPS_ASYNC_TEST_PASSWORD',
    'VPS_ASYNC_TEST_DBNAME',
    'VPS_ASYNC_TEST_SSLMODE',
    'VPS_ASYNC_TEST_CHANNEL_BINDING'
)
foreach ($name in $required) {
    if ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name))) {
        throw "[vtable-stand] required runtime environment variable is missing: $name"
    }
}

$buildPath = Join-Path $Root $BuildDirectory
$hostExecutable = Join-Path $buildPath 'vps_extension_host_test.exe'
$extension = Join-Path $buildPath 'virtualpostgresql.dll'
if (!(Test-Path -LiteralPath $hostExecutable) -or
    !(Test-Path -LiteralPath $extension)) {
    throw '[vtable-stand] host or extension binary is missing'
}

$parts = @(
    "host=$env:VPS_ASYNC_TEST_HOST",
    "port=$env:VPS_ASYNC_TEST_PORT",
    "user=$env:VPS_ASYNC_TEST_USER",
    "password=$env:VPS_ASYNC_TEST_PASSWORD",
    "dbname=$env:VPS_ASYNC_TEST_DBNAME",
    "sslmode=$env:VPS_ASYNC_TEST_SSLMODE",
    "channel_binding=$env:VPS_ASYNC_TEST_CHANNEL_BINDING"
)
$previous = [Environment]::GetEnvironmentVariable('VPS_VTAB_TEST_CONNSTR')
try {
    [Environment]::SetEnvironmentVariable('VPS_VTAB_TEST_CONNSTR',
                                           ($parts -join ' '))
    & $hostExecutable $extension
    if ($LASTEXITCODE -ne 0) { throw '[vtable-stand] read contour failed' }
} finally {
    [Environment]::SetEnvironmentVariable('VPS_VTAB_TEST_CONNSTR', $previous)
}

$bulk = [Environment]::GetEnvironmentVariable('VPS_VTAB_TEST_BULK') -eq '1'
Write-Output "vtable_stand status=passed sources=table,view,query planner=predicate,in,projection,order,limit,cost cursors=2 bulk=$bulk"
