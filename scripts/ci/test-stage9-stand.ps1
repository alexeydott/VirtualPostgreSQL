[CmdletBinding()]
param(
    [ValidateSet('Local', 'Tls')]
    [string]$Contour = 'Local',
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$BuildDirectory = 'build/stage1-msvc-x64-release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$prefix = if ($Contour -eq 'Local') { 'VPS_SESSION_TEST' } else { 'VPS_TLS_TEST' }
$temporary = [Collections.Generic.Dictionary[string, object]]::new()

function Set-Stage9Environment([string]$Name, [AllowNull()][string]$Value) {
    if (-not $temporary.ContainsKey($Name)) {
        $temporary[$Name] = [Environment]::GetEnvironmentVariable(
            $Name, [EnvironmentVariableTarget]::Process)
    }
    [Environment]::SetEnvironmentVariable(
        $Name, $Value, [EnvironmentVariableTarget]::Process)
}

try {
    foreach ($suffix in @('HOST', 'PORT', 'USER', 'PASSWORD', 'DBNAME')) {
        $sourceName = "${prefix}_${suffix}"
        $value = [Environment]::GetEnvironmentVariable(
            $sourceName, [EnvironmentVariableTarget]::Process)
        if ([string]::IsNullOrEmpty($value)) {
            throw "[stage9-stand] required runtime environment variable is missing: $sourceName"
        }
        Set-Stage9Environment "VPS_ASYNC_TEST_${suffix}" $value
        $value = $null
    }

    if ($Contour -eq 'Local') {
        Set-Stage9Environment 'VPS_ASYNC_TEST_SSLMODE' 'disable'
        Set-Stage9Environment 'VPS_ASYNC_TEST_CHANNEL_BINDING' 'prefer'
        Set-Stage9Environment 'VPS_ASYNC_TEST_FIXTURE' '1'
        Set-Stage9Environment 'VPS_ASYNC_TEST_NETWORK_LOSS' '1'
        Set-Stage9Environment 'VPS_VTAB_TEST_BULK' '1'
    } else {
        $tlsMode = [Environment]::GetEnvironmentVariable(
            'VPS_TLS_TEST_SSLMODE', [EnvironmentVariableTarget]::Process)
        if ([string]::IsNullOrEmpty($tlsMode)) { $tlsMode = 'verify-full' }
        if ($tlsMode -notin @('require', 'verify-ca', 'verify-full')) {
            throw '[stage9-stand] invalid VPS_TLS_TEST_SSLMODE'
        }
        Set-Stage9Environment 'VPS_ASYNC_TEST_SSLMODE' $tlsMode
        Set-Stage9Environment 'VPS_ASYNC_TEST_CHANNEL_BINDING' 'prefer'
        Set-Stage9Environment 'VPS_ASYNC_TEST_FIXTURE' '1'
        Set-Stage9Environment 'VPS_ASYNC_TEST_NETWORK_LOSS' $null
        Set-Stage9Environment 'VPS_VTAB_TEST_BULK' $null
    }

    $build = Join-Path $Root $BuildDirectory
    $probe = Join-Path $build 'vps_libpq_client_connect_integration_test.exe'
    if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
        throw '[stage9-stand] async integration probe is missing'
    }
    & $probe
    if ($LASTEXITCODE -ne 0) { throw '[stage9-stand] async contour failed' }

    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-vtable-stand.ps1') `
        -Root $Root -BuildDirectory $BuildDirectory
    if ($LASTEXITCODE -ne 0) { throw '[stage9-stand] vtable contour failed' }
} finally {
    foreach ($entry in $temporary.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable(
            $entry.Key, $entry.Value, [EnvironmentVariableTarget]::Process)
    }
}

Write-Output "stage9_stand contour=$Contour fixture_rows=3 bulk=$($Contour -eq 'Local') status=passed"
