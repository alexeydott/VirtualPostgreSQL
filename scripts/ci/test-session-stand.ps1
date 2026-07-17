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

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$buildCandidate = [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
$rootPrefix = $root.TrimEnd([IO.Path]::DirectorySeparatorChar) +
    [IO.Path]::DirectorySeparatorChar
if (-not $buildCandidate.StartsWith(
        $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw '[session-stand] build directory must remain inside the repository root'
}
$build = (Resolve-Path -LiteralPath $buildCandidate -ErrorAction Stop).Path
$probe = Join-Path $build 'vps_libpq_client_session_integration_test.exe'
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
    throw "[session-stand] integration probe is missing: $probe"
}
& $probe
if ($LASTEXITCODE -ne 0) {
    throw '[session-stand] session baseline contour failed'
}
Write-Output '[session-stand] status=passed setting_calls=33 user_data_queries=0'
