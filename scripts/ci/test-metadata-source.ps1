[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'metadata-source'
$rootPath = Assert-VpsSafeRoot -Root $Root
$portableHeaders = @(
    'src/vps_metadata.h',
    'src/vps_type_registry.h',
    'src/vps_schema_fingerprint.h'
)
$portableSources = @(
    'src/vps_metadata.c',
    'src/vps_metadata_key.c',
    'src/vps_metadata_policy.c',
    'src/vps_type_registry.c',
    'src/vps_schema_fingerprint.c'
)
$failed = $false

foreach ($relative in $portableHeaders + $portableSources) {
    $path = Join-Path $rootPath $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'missing_source' -RelativePath $relative
        $failed = $true
        continue
    }
    $content = Get-Content -LiteralPath $path -Raw
    if ($content -match '(?im)^\s*#\s*include\s*[<"](?:libpq-fe\.h|sqlite3(?:ext)?\.h|windows\.h|winsock2?\.h|unistd\.h|pthread\.h)[>"]') {
        Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'portable_header_leak' -RelativePath $relative
        $failed = $true
    }
    if ($content -match '\bPQ[A-Z][A-Za-z0-9_]*\s*\(') {
        Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'libpq_call_outside_adapter' -RelativePath $relative
        $failed = $true
    }
}

$catalogPath = Join-Path $rootPath 'src/vps_metadata.c'
$catalog = Get-Content -LiteralPath $catalogPath -Raw
if ($catalog -match '(?i)\b(?:FROM|JOIN)\s+(?!pg_catalog\.)pg_[a-z_]+\b' -or
    $catalog -match '(?i)(?<!pg_catalog\.)\b(?:pg_get_expr|md5)\s*\(' -or
    $catalog -notmatch '\$1::pg_catalog\.' -or
    $catalog -match '(?i)\b(?:sprintf|snprintf|strcat)\s*\(') {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'unsafe_catalog_sql' -RelativePath 'src/vps_metadata.c'
    $failed = $true
}

if ($failed) { exit 1 }
Write-VpsCiEvent -Gate $gate -Level info -Status passed -Detail "portable_files=$($portableHeaders.Count + $portableSources.Count)"
