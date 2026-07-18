[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$MatrixRoot = 'build/stage15-pg-matrix',
    [string]$VisualStudioRoot = 'D:\Visual Studio2022'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')
$rootPath = Assert-VpsSafeRoot -Root $Root
$matrixPath = Join-Path $rootPath $MatrixRoot
$gate = 'pg-matrix'
$started = [Diagnostics.Stopwatch]::StartNew()
$servers = @(15, 16, 17, 18 | ForEach-Object {
    @{ Major=$_; Port=5500 + $_; Root=(Join-Path $matrixPath "pg$_\pgsql");
       Data=(Join-Path $matrixPath "data-pg$_") }
})
$startedServers = [Collections.Generic.List[object]]::new()
$savedEnvironment = @{}
$environmentNames = @('VPS_ASYNC_TEST_HOST','VPS_ASYNC_TEST_PORT',
    'VPS_ASYNC_TEST_USER','VPS_ASYNC_TEST_PASSWORD','VPS_ASYNC_TEST_DBNAME',
    'VPS_ASYNC_TEST_SSLMODE','VPS_ASYNC_TEST_CHANNEL_BINDING',
    'VPS_ASYNC_TEST_FIXTURE','VPS_ASYNC_TEST_NETWORK_LOSS')
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
    foreach ($preset in @('msvc-x86-debug', 'msvc-x64-release')) {
        & pwsh -NoProfile -File (Join-Path $rootPath 'scripts\build-stage1.ps1') `
            -Preset $preset -VisualStudioRoot $VisualStudioRoot
        if ($LASTEXITCODE -ne 0) { throw "[pg-matrix] client build failed: $preset" }
    }
    foreach ($server in $servers) {
        $bin = Join-Path $server.Root 'bin'
        $initdb = Join-Path $bin 'initdb.exe'
        $pgCtl = Join-Path $bin 'pg_ctl.exe'
        $psql = Join-Path $bin 'psql.exe'
        if (-not (Test-Path -LiteralPath $initdb -PathType Leaf)) {
            throw "[pg-matrix] mandatory server unavailable: PG$($server.Major)"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $server.Data 'PG_VERSION'))) {
            & $initdb -D $server.Data -U postgres -A trust --no-locale -E UTF8
            if ($LASTEXITCODE -ne 0) { throw "[pg-matrix] initdb failed: PG$($server.Major)" }
        }
        $serverLog = Join-Path $matrixPath "pg$($server.Major)-server.log"
        & $pgCtl -D $server.Data -l $serverLog -o `
            "-p $($server.Port) -h 127.0.0.1 -c fsync=off -c synchronous_commit=off -c full_page_writes=off" start -w
        if ($LASTEXITCODE -ne 0) { throw "[pg-matrix] start failed: PG$($server.Major)" }
        $startedServers.Add($server)
        $version = (& $psql -h 127.0.0.1 -p $server.Port -U postgres -d postgres `
            -Atqc 'SHOW server_version_num').Trim()
        if ($LASTEXITCODE -ne 0 -or -not $version.StartsWith("$($server.Major)")) {
            throw "[pg-matrix] server major attestation failed: expected=$($server.Major)"
        }
        $env:VPS_ASYNC_TEST_HOST = '127.0.0.1'
        $env:VPS_ASYNC_TEST_PORT = [string]$server.Port
        $env:VPS_ASYNC_TEST_USER = 'postgres'
        $env:VPS_ASYNC_TEST_PASSWORD = [guid]::NewGuid().ToString('N')
        $env:VPS_ASYNC_TEST_DBNAME = 'postgres'
        $env:VPS_ASYNC_TEST_SSLMODE = 'disable'
        $env:VPS_ASYNC_TEST_CHANNEL_BINDING = 'disable'
        $env:VPS_ASYNC_TEST_FIXTURE = '1'
        $env:VPS_ASYNC_TEST_NETWORK_LOSS = $null
        foreach ($client in @(
            @{Arch='x86'; Path='build/stage1-msvc-x86-debug/vps_libpq_client_connect_integration_test.exe'},
            @{Arch='x64'; Path='build/stage1-msvc-x64-release/vps_libpq_client_connect_integration_test.exe'})) {
            & (Join-Path $rootPath $client.Path)
            if ($LASTEXITCODE -ne 0) {
                throw "[pg-matrix] integration failed: server=$($server.Major) client=$($client.Arch)"
            }
            Write-VpsCiEvent -Gate $gate -Level info -Status passed `
                -Detail "server_major=$($server.Major),server_version_num=$version,client_arch=$($client.Arch),scenario=async_fixture"
        }
    }
} finally {
    foreach ($server in $startedServers) {
        & (Join-Path $server.Root 'bin\pg_ctl.exe') -D $server.Data stop -m fast -w | Out-Null
    }
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name])
    }
}
$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status skipped `
    -Detail 'server_major=14,contour=optional_legacy,reason=package_not_provisioned'
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "duration_ms=$($started.ElapsedMilliseconds),mandatory_majors=4,client_architectures=2,rows=8"
