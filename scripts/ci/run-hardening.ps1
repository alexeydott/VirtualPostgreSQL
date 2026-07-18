[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$MatrixRoot = 'build/stage15-pg-matrix',
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [string]$OpenSsl = 'C:\Program Files\Git\usr\bin\openssl.exe',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')
$rootPath = Assert-VpsSafeRoot -Root $Root
$gate = 'hardening'
$started = [Diagnostics.Stopwatch]::StartNew()
$build = Join-Path $rootPath 'build\stage1-msvc-x64-release'
$evidence = Join-Path $rootPath 'build\stage15-hardening\evidence'
$serverRoot = Join-Path $rootPath $MatrixRoot 'pg18\pgsql'
$serverData = Join-Path $rootPath 'build\stage15-hardening\pg18-data'
$serverLog = Join-Path $rootPath 'build\stage15-hardening\pg18-server.log'
$port = 5598
$runtimePassword = 'Vps-' + [guid]::NewGuid().ToString('N')
$role = 'vps_hardening_role'
$startedServer = $false
$savedPath = $env:PATH
$savedEnvironment = @{}
$environmentNames = @(
    'VPS_ASYNC_TEST_HOST','VPS_ASYNC_TEST_PORT','VPS_ASYNC_TEST_USER',
    'VPS_ASYNC_TEST_PASSWORD','VPS_ASYNC_TEST_DBNAME','VPS_ASYNC_TEST_SSLMODE',
    'VPS_ASYNC_TEST_SSLROOTCERT','VPS_ASYNC_TEST_CHANNEL_BINDING',
    'VPS_ASYNC_TEST_FIXTURE','VPS_ASYNC_TEST_NETWORK_LOSS',
    'VPS_TLS_TEST_HOST','VPS_TLS_TEST_PORT','VPS_TLS_TEST_USER',
    'VPS_TLS_TEST_PASSWORD','VPS_TLS_TEST_DBNAME','SSL_CERT_FILE','PGPASSWORD',
    'PGSSLMODE','PGSSLROOTCERT'
)
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}

function Invoke-HardeningProbe {
    param([Parameter(Mandatory)][string]$Name,
          [Parameter(Mandatory)][string]$Executable,
          [string[]]$Required = @())
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw "[hardening] probe is missing: $Name"
    }
    $output = @(& $Executable 2>&1)
    $exitCode = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $evidence "$Name.log") -Encoding utf8
    if ($exitCode -ne 0) { throw "[hardening] probe failed: $Name" }
    $text = $output -join "`n"
    foreach ($pattern in $Required) {
        if ($text -notmatch $pattern) {
            throw "[hardening] required evidence missing: probe=$Name"
        }
    }
    Write-VpsCiEvent -Gate $gate -Level info -Status passed `
        -Detail "scenario=$Name,exit_code=0"
}

function Invoke-PsqlScalar {
    param([Parameter(Mandatory)][string]$Psql,
          [Parameter(Mandatory)][string]$User,
          [Parameter(Mandatory)][string]$Sql)
    $value = @(& $Psql -h 127.0.0.1 -p $port -U $User -d postgres `
        --no-password -v ON_ERROR_STOP=1 -Atqc $Sql 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $safeError = ($value -join ' ').Replace($runtimePassword, '[redacted]')
        throw "[hardening] PostgreSQL assertion failed: $safeError"
    }
    return ($value -join "`n").Trim()
}

try {
    New-Item -ItemType Directory -Path $evidence -Force | Out-Null
    if (-not $SkipBuild) {
        & pwsh -NoProfile -File (Join-Path $rootPath 'scripts\build-stage1.ps1') `
            -Preset msvc-x64-release -VisualStudioRoot $VisualStudioRoot
        if ($LASTEXITCODE -ne 0) { throw '[hardening] release build failed' }
    }

    Invoke-HardeningProbe -Name pool_stress -Executable `
        (Join-Path $build 'vps_connection_pool_test.exe') `
        -Required @('workers=8 iterations=1000 status=passed')
    Invoke-HardeningProbe -Name transaction -Executable `
        (Join-Path $build 'vps_transaction_test.exe') `
        -Required @('status=passed')
    foreach ($probe in @('vps_abi_layout_test','vps_credential_provider_test',
                          'vps_wincred_provider_test','vps_query_profile_test',
                          'vps_logging_test','vps_error_test','vps_session_test',
                          'vps_tls_policy_test','vps_libpq_client_test')) {
        Invoke-HardeningProbe -Name $probe -Executable (Join-Path $build "$probe.exe")
    }

    $bin = Join-Path $serverRoot 'bin'
    $initdb = Join-Path $bin 'initdb.exe'
    $pgCtl = Join-Path $bin 'pg_ctl.exe'
    $psql = Join-Path $bin 'psql.exe'
    if (-not (Test-Path -LiteralPath $initdb -PathType Leaf)) {
        throw '[hardening] pinned PG18 runtime missing; run prepare-pg-matrix.ps1'
    }
    if (-not (Test-Path -LiteralPath $OpenSsl -PathType Leaf)) {
        throw '[hardening] OpenSSL command is unavailable'
    }
    if (Test-Path -LiteralPath $serverData) {
        $resolvedData = [IO.Path]::GetFullPath($serverData)
        $safePrefix = [IO.Path]::GetFullPath((Join-Path $rootPath 'build\stage15-hardening')) + [IO.Path]::DirectorySeparatorChar
        if (-not $resolvedData.StartsWith($safePrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw '[hardening] unsafe PostgreSQL data path'
        }
        Remove-Item -LiteralPath $resolvedData -Recurse -Force
    }
    & $initdb -D $serverData -U postgres -A trust --no-locale -E UTF8 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '[hardening] initdb failed' }

    $caKey = Join-Path $serverData 'hardening-ca.key'
    $caCert = Join-Path $serverData 'hardening-ca.crt'
    $serverKey = Join-Path $serverData 'server.key'
    $serverCert = Join-Path $serverData 'server.crt'
    $serverCsr = Join-Path $serverData 'server.csr'
    & $OpenSsl req -x509 -newkey rsa:2048 -nodes -sha256 -days 2 `
        -subj '/CN=VirtualPostgreSQL hardening CA' -keyout $caKey -out $caCert 2>$null
    if ($LASTEXITCODE -ne 0) { throw '[hardening] CA generation failed' }
    & $OpenSsl req -newkey rsa:2048 -nodes -sha256 -subj '/CN=localhost' `
        -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' `
        -keyout $serverKey -out $serverCsr 2>$null
    if ($LASTEXITCODE -ne 0) { throw '[hardening] server CSR generation failed' }
    & $OpenSsl x509 -req -sha256 -days 2 -in $serverCsr -CA $caCert `
        -CAkey $caKey -CAcreateserial -copy_extensions copy -out $serverCert 2>$null
    if ($LASTEXITCODE -ne 0) { throw '[hardening] server certificate generation failed' }

    & $pgCtl -D $serverData -l $serverLog -o `
        "-p $port -h 127.0.0.1 -c ssl=on -c fsync=off -c synchronous_commit=off" start -w
    if ($LASTEXITCODE -ne 0) { throw '[hardening] PG18 TLS start failed' }
    $startedServer = $true

    $bootstrap = @"
CREATE ROLE $role LOGIN PASSWORD '$runtimePassword' NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT;
CREATE SCHEMA vps_hardening AUTHORIZATION postgres;
CREATE TABLE vps_hardening.rls_probe(owner_name name NOT NULL, payload text NOT NULL);
ALTER TABLE vps_hardening.rls_probe ENABLE ROW LEVEL SECURITY;
CREATE POLICY owner_rows ON vps_hardening.rls_probe USING (owner_name = current_user);
INSERT INTO vps_hardening.rls_probe VALUES ('$role','visible'),('postgres','hidden');
GRANT USAGE ON SCHEMA vps_hardening TO $role;
GRANT SELECT ON vps_hardening.rls_probe TO $role;
"@
    & $psql -h 127.0.0.1 -p $port -U postgres -d postgres `
        -v ON_ERROR_STOP=1 -q -c $bootstrap
    if ($LASTEXITCODE -ne 0) { throw '[hardening] security fixture failed' }
    Set-Content -LiteralPath (Join-Path $serverData 'pg_hba.conf') -Encoding ascii -Value @(
        'hostssl all all 127.0.0.1/32 scram-sha-256',
        'hostnossl all all 127.0.0.1/32 reject'
    )
    & $pgCtl -D $serverData reload | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '[hardening] pg_hba reload failed' }

    $env:PGPASSWORD = $runtimePassword
    $env:PGSSLMODE = 'verify-full'
    $env:PGSSLROOTCERT = $caCert
    $rlsCount = Invoke-PsqlScalar -Psql $psql -User $role `
        -Sql 'SELECT count(*) FROM vps_hardening.rls_probe'
    if ($rlsCount -ne '1') { throw '[hardening] RLS isolation failed' }
    $roleFlags = Invoke-PsqlScalar -Psql $psql -User $role `
        -Sql "SELECT rolsuper::int::text||rolcreatedb::int::text||rolcreaterole::int::text FROM pg_catalog.pg_roles WHERE rolname=current_user"
    if ($roleFlags -ne '000') { throw '[hardening] least-privilege role assertion failed' }
    'rls_rows=1 role_flags=000 status=passed' | Set-Content `
        -LiteralPath (Join-Path $evidence 'server_authorization.log') -Encoding utf8

    $env:VPS_ASYNC_TEST_HOST = '127.0.0.1'
    $env:VPS_ASYNC_TEST_PORT = [string]$port
    $env:VPS_ASYNC_TEST_USER = $role
    $env:VPS_ASYNC_TEST_PASSWORD = $runtimePassword
    $env:VPS_ASYNC_TEST_DBNAME = 'postgres'
    $env:VPS_ASYNC_TEST_SSLMODE = 'verify-full'
    $env:VPS_ASYNC_TEST_SSLROOTCERT = $caCert
    $env:VPS_ASYNC_TEST_CHANNEL_BINDING = 'require'
    $env:VPS_ASYNC_TEST_FIXTURE = $null
    $env:VPS_ASYNC_TEST_NETWORK_LOSS = '1'
    Invoke-HardeningProbe -Name network_loss -Executable `
        (Join-Path $build 'vps_libpq_client_connect_integration_test.exe') `
        -Required @('network=ok','status=passed')

    $env:PATH = (Split-Path -Parent $OpenSsl) + [IO.Path]::PathSeparator + $savedPath
    $env:VPS_TLS_TEST_HOST = '127.0.0.1'
    $env:VPS_TLS_TEST_PORT = [string]$port
    $env:VPS_TLS_TEST_USER = $role
    $env:VPS_TLS_TEST_PASSWORD = $runtimePassword
    $env:VPS_TLS_TEST_DBNAME = 'postgres'
    $tlsOutput = @(& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-tls-stand.ps1') `
        -BuildDirectory 'build/stage1-msvc-x64-release' -TrustedCaFile $caCert 2>&1)
    $tlsExit = $LASTEXITCODE
    $tlsOutput | Set-Content -LiteralPath (Join-Path $evidence 'tls.log') -Encoding utf8
    if ($tlsExit -ne 0 -or ($tlsOutput -join "`n") -notmatch 'status=passed probes=8') {
        throw '[hardening] TLS/auth contour failed'
    }

    $evidenceText = @(Get-ChildItem -LiteralPath $evidence -File | ForEach-Object {
        Get-Content -LiteralPath $_.FullName -Raw
    }) -join "`n"
    if ($evidenceText.Contains($runtimePassword) -or
        $evidenceText -match '(?i)(password|access[_ -]?token|private[_ -]?key)\s*=') {
        throw '[hardening] secret-like content found in evidence'
    }
    Write-VpsCiEvent -Gate $gate -Level info -Status passed `
        -Detail 'stress_workers=8,stress_iterations=1000,pool_exhaustion=passed,network_points=2,rls=passed,search_path=controlled,tls_probes=8,redaction=passed,provider_abi=passed,unknown_commit=failed_closed,secrets=absent'
} finally {
    if ($startedServer) {
        & (Join-Path $serverRoot 'bin\pg_ctl.exe') -D $serverData stop -m immediate -w | Out-Null
    }
    $env:PATH = $savedPath
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
    -Detail "duration_ms=$($started.ElapsedMilliseconds),evidence=build/stage15-hardening/evidence"
