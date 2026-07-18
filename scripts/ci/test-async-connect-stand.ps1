[CmdletBinding()]
param(
    [ValidateSet('Local', 'Tls')]
    [string]$Contour = 'Local',
    [string]$BuildDirectory = 'build/stage1-msvc-x64-release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$prefix = if ($Contour -eq 'Local') { 'VPS_SESSION_TEST' } else { 'VPS_TLS_TEST' }
$requiredSuffixes = @('HOST', 'PORT', 'USER', 'PASSWORD', 'DBNAME')
$temporaryNames = [Collections.Generic.List[string]]::new()

try {
    foreach ($suffix in $requiredSuffixes) {
        $sourceName = "${prefix}_${suffix}"
        $targetName = "VPS_ASYNC_TEST_${suffix}"
        $value = [Environment]::GetEnvironmentVariable(
            $sourceName, [EnvironmentVariableTarget]::Process)
        if ([string]::IsNullOrEmpty($value)) {
            throw "[async-connect-stand] required runtime environment variable is missing: $sourceName"
        }
        [Environment]::SetEnvironmentVariable(
            $targetName, $value, [EnvironmentVariableTarget]::Process)
        $temporaryNames.Add($targetName)
        $value = $null
    }

    $sslmode = if ($Contour -eq 'Local') { 'disable' } else { 'verify-full' }
    $channelBinding = if ($Contour -eq 'Local') { 'disable' } else { 'prefer' }
    $sslrootcert = if ($Contour -eq 'Local') {
        $null
    } else {
        $rootOverride = [Environment]::GetEnvironmentVariable(
            "${prefix}_SSLROOTCERT", [EnvironmentVariableTarget]::Process)
        if ([string]::IsNullOrEmpty($rootOverride)) { 'system' }
        else { $rootOverride }
    }
    foreach ($entry in @{
            VPS_ASYNC_TEST_SSLMODE = $sslmode
            VPS_ASYNC_TEST_CHANNEL_BINDING = $channelBinding
            VPS_ASYNC_TEST_SSLROOTCERT = $sslrootcert
        }.GetEnumerator()) {
        if ($null -ne $entry.Value) {
            [Environment]::SetEnvironmentVariable(
                $entry.Key, $entry.Value, [EnvironmentVariableTarget]::Process)
            $temporaryNames.Add($entry.Key)
        }
    }

    $fixture = [Environment]::GetEnvironmentVariable(
        "${prefix}_FIXTURE", [EnvironmentVariableTarget]::Process)
    if (-not [string]::IsNullOrEmpty($fixture)) {
        [Environment]::SetEnvironmentVariable(
            'VPS_ASYNC_TEST_FIXTURE', $fixture,
            [EnvironmentVariableTarget]::Process)
        $temporaryNames.Add('VPS_ASYNC_TEST_FIXTURE')
    }
    $networkLoss = [Environment]::GetEnvironmentVariable(
        "${prefix}_NETWORK_LOSS", [EnvironmentVariableTarget]::Process)
    if (-not [string]::IsNullOrEmpty($networkLoss)) {
        [Environment]::SetEnvironmentVariable(
            'VPS_ASYNC_TEST_NETWORK_LOSS', $networkLoss,
            [EnvironmentVariableTarget]::Process)
        $temporaryNames.Add('VPS_ASYNC_TEST_NETWORK_LOSS')
    }
    foreach ($suffix in @('METADATA_SCHEMA', 'METADATA_TABLE')) {
        $value = [Environment]::GetEnvironmentVariable(
            "${prefix}_${suffix}", [EnvironmentVariableTarget]::Process)
        if (-not [string]::IsNullOrEmpty($value)) {
            $targetName = "VPS_ASYNC_TEST_${suffix}"
            [Environment]::SetEnvironmentVariable(
                $targetName, $value, [EnvironmentVariableTarget]::Process)
            $temporaryNames.Add($targetName)
        }
    }

    $root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $buildCandidate = [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
    $rootPrefix = $root.TrimEnd([IO.Path]::DirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    if (-not $buildCandidate.StartsWith(
            $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw '[async-connect-stand] build directory must remain inside the repository root'
    }
    $build = (Resolve-Path -LiteralPath $buildCandidate -ErrorAction Stop).Path
    $probe = Join-Path $build 'vps_libpq_client_connect_integration_test.exe'
    if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
        throw "[async-connect-stand] integration probe is missing: $probe"
    }
    & $probe
    if ($LASTEXITCODE -ne 0) {
        throw "[async-connect-stand] $Contour contour failed"
    }
} finally {
    foreach ($name in $temporaryNames) {
        Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue
    }
}

$queryCount = if ([string]::IsNullOrEmpty($fixture)) { 0 } else { 2 }
Write-Output "[async-connect-stand] contour=$Contour status=passed user_data_queries=$queryCount"
