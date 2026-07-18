[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$MatrixRoot = 'build/stage15-pg-matrix',
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')
$rootPath = Assert-VpsSafeRoot -Root $Root
$gate = 'performance'
$started = [Diagnostics.Stopwatch]::StartNew()
$build = Join-Path $rootPath 'build\stage1-msvc-x64-release'
$evidence = Join-Path $rootPath 'build\stage15-performance\evidence'
$serverRoot = Join-Path $rootPath $MatrixRoot 'pg18\pgsql'
$serverData = Join-Path $rootPath 'build\stage15-performance\pg18-data'
$serverLog = Join-Path $rootPath 'build\stage15-performance\pg18-server.log'
$port = 5599
$runtimePassword = 'Vps-' + [guid]::NewGuid().ToString('N')
$startedServer = $false
$savedEnvironment = @{}
$environmentNames = @(
    'VPS_ASYNC_TEST_HOST','VPS_ASYNC_TEST_PORT','VPS_ASYNC_TEST_USER',
    'VPS_ASYNC_TEST_PASSWORD','VPS_ASYNC_TEST_DBNAME','VPS_ASYNC_TEST_SSLMODE',
    'VPS_ASYNC_TEST_CHANNEL_BINDING','VPS_ASYNC_TEST_FIXTURE',
    'VPS_ASYNC_TEST_NETWORK_LOSS','VPS_ASYNC_TEST_PERFORMANCE',
    'VPS_VTAB_TEST_CONNSTR','VPS_VTAB_TEST_BULK','VPS_VTAB_TEST_PERFORMANCE'
)
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}

function Invoke-PerformanceProbe {
    param([Parameter(Mandatory)][string]$Name,
          [Parameter(Mandatory)][string]$Executable,
          [string[]]$Arguments = @(),
          [string[]]$Required = @())
    $output = @(& $Executable @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $evidence "$Name.log") -Encoding utf8
    if ($exitCode -ne 0) { throw "[performance] probe failed: $Name" }
    $text = $output -join "`n"
    foreach ($pattern in $Required) {
        if ($text -notmatch $pattern) {
            throw "[performance] evidence missing: probe=$Name"
        }
    }
    return $text
}

try {
    New-Item -ItemType Directory -Path $evidence -Force | Out-Null
    if (-not $SkipBuild) {
        & pwsh -NoProfile -File (Join-Path $rootPath 'scripts\build-stage1.ps1') `
            -Preset msvc-x64-release -VisualStudioRoot $VisualStudioRoot
        if ($LASTEXITCODE -ne 0) { throw '[performance] release build failed' }
    }
    $bin = Join-Path $serverRoot 'bin'
    $initdb = Join-Path $bin 'initdb.exe'
    $pgCtl = Join-Path $bin 'pg_ctl.exe'
    $psql = Join-Path $bin 'psql.exe'
    if (-not (Test-Path -LiteralPath $initdb -PathType Leaf)) {
        throw '[performance] pinned PG18 runtime missing; run prepare-pg-matrix.ps1'
    }
    if (Test-Path -LiteralPath $serverData) {
        Remove-Item -LiteralPath $serverData -Recurse -Force
    }
    & $initdb -D $serverData -U postgres -A trust --no-locale -E UTF8 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '[performance] initdb failed' }
    & $pgCtl -D $serverData -l $serverLog -o `
        "-p $port -h 127.0.0.1 -c fsync=off -c synchronous_commit=off -c full_page_writes=off" start -w
    if ($LASTEXITCODE -ne 0) { throw '[performance] PG18 start failed' }
    $startedServer = $true
    $serverVersion = (& $psql -h 127.0.0.1 -p $port -U postgres -d postgres `
        -Atqc 'SHOW server_version_num').Trim()
    if ($LASTEXITCODE -ne 0 -or -not $serverVersion.StartsWith('18')) {
        throw '[performance] server attestation failed'
    }

    $env:VPS_ASYNC_TEST_HOST = '127.0.0.1'
    $env:VPS_ASYNC_TEST_PORT = [string]$port
    $env:VPS_ASYNC_TEST_USER = 'postgres'
    $env:VPS_ASYNC_TEST_PASSWORD = $runtimePassword
    $env:VPS_ASYNC_TEST_DBNAME = 'postgres'
    $env:VPS_ASYNC_TEST_SSLMODE = 'disable'
    $env:VPS_ASYNC_TEST_CHANNEL_BINDING = 'disable'
    $env:VPS_ASYNC_TEST_FIXTURE = $null
    $env:VPS_ASYNC_TEST_NETWORK_LOSS = $null
    $env:VPS_ASYNC_TEST_PERFORMANCE = '1'
    $coldSamples = [Collections.Generic.List[int]]::new()
    for ($sample = 1; $sample -le 3; ++$sample) {
        $text = Invoke-PerformanceProbe -Name "connection-$sample" -Executable `
            (Join-Path $build 'vps_libpq_client_connect_integration_test.exe') `
            -Required @('connection_performance .*status=passed','async_connect_probe status=passed')
        if ($text -notmatch 'connection_performance cold_ms=(\d+) warm_total_ms=(\d+) warm_iterations=100') {
            throw '[performance] connection metrics could not be parsed'
        }
        $coldSamples.Add([int]$Matches[1])
    }
    $coldP95 = ($coldSamples | Measure-Object -Maximum).Maximum
    if ($coldP95 -gt 5000) { throw '[performance] cold p95 threshold exceeded' }

    $env:VPS_VTAB_TEST_CONNSTR = `
        "host=127.0.0.1 port=$port user=postgres dbname=postgres sslmode=disable"
    $env:VPS_VTAB_TEST_BULK = $null
    $env:VPS_VTAB_TEST_PERFORMANCE = $null
    $hostText = Invoke-PerformanceProbe -Name host-network -Executable `
        (Join-Path $build 'vps_extension_host_test.exe') `
        -Arguments @((Join-Path $build 'virtualpostgresql.dll')) `
        -Required @('operation=pushdown-equivalence .*status=passed')
    $streamText = Invoke-PerformanceProbe -Name stream-million-row -Executable `
        (Join-Path $build 'vps_client_stream_performance_test.exe') `
        -Required @('client_stream rows=1000000 .*rss_delta_bytes=\d+ .*status=passed')

    $materialization = [Collections.Generic.List[string]]::new()
    for ($sample = 1; $sample -le 3; ++$sample) {
        $materialization.Add((Invoke-PerformanceProbe `
            -Name "materialization-$sample" `
            -Executable (Join-Path $build 'vps_materialization_performance_test.exe') `
            -Required @('samples=3 probes=100 .*status=passed')))
    }
    $fingerprintInput = "server=$serverVersion;arch=x64;preset=msvc-x64-release;rows=1000000;samples=3"
    $fingerprintBytes = [Text.Encoding]::UTF8.GetBytes($fingerprintInput)
    $fingerprint = [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData($fingerprintBytes)).ToLowerInvariant()
    "environment_fingerprint=$fingerprint server_major=18 arch=x64 samples=3 status=passed" |
        Set-Content -LiteralPath (Join-Path $evidence 'environment.log') -Encoding utf8
    $evidenceText = @(Get-ChildItem -LiteralPath $evidence -File | ForEach-Object {
        Get-Content -LiteralPath $_.FullName -Raw
    }) -join "`n"
    if ($evidenceText.Contains($runtimePassword)) {
        throw '[performance] secret found in evidence'
    }
    Write-VpsCiEvent -Gate $gate -Level info -Status passed `
        -Detail "cold_samples=3,cold_p95_ms=$coldP95,warm_pool=faster,stream_rows=1000000,rss=bounded,pushdown=equivalent,materialization_samples=3,materialization_p50_speedup_ge=10,environment_fingerprint=$fingerprint"
} finally {
    if ($startedServer) {
        & (Join-Path $serverRoot 'bin\pg_ctl.exe') -D $serverData stop -m fast -w | Out-Null
    }
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name])
    }
    $runtimePassword = $null
    if (Test-Path -LiteralPath $serverData) {
        Remove-Item -LiteralPath $serverData -Recurse -Force
    }
}
$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "duration_ms=$($started.ElapsedMilliseconds),evidence=build/stage15-performance/evidence"
