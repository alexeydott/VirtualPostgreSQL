[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'query-source'
$rootPath = Assert-VpsSafeRoot -Root $Root
$required = @(
    'src/vps_query_source.c',
    'src/vps_query_profile.c',
    'src/vps_query_validation.c',
    'src/vps_query_metadata.c',
    'src/vps_query_indexes.c',
    'src/vps_query_boundary.c',
    'src/vps_query_fingerprint.c',
    'tests/unit/vps_query_source_test.c',
    'tests/unit/vps_query_profile_test.c',
    'tests/unit/vps_query_validation_test.c',
    'tests/unit/vps_query_metadata_test.c',
    'tests/unit/vps_query_boundary_test.c',
    'tests/unit/vps_query_fingerprint_test.c',
    'docs/query-sources.md'
)

foreach ($relative in $required) {
    $path = Join-Path $rootPath $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Write-VpsCiEvent -Gate $gate -Level error -Status failed `
            -FailureClass 'missing_source' -RelativePath $relative
        exit 1
    }
}

$portableFiles = Get-ChildItem -LiteralPath (Join-Path $rootPath 'src') `
    -File -Filter 'vps_query_*' | Where-Object { $_.Extension -in @('.c', '.h') }
foreach ($file in $portableFiles) {
    $content = Get-Content -LiteralPath $file.FullName -Raw
    if ($content -match '#\s*include\s*[<"](?:libpq-fe|windows|winsock2)\.h') {
        Write-VpsCiEvent -Gate $gate -Level error -Status failed `
            -FailureClass 'portable_header_leak' `
            -RelativePath (Get-VpsRelativePath -Root $rootPath -Path $file.FullName)
        exit 1
    }
    if ($content -match '\bPQ[A-Za-z0-9_]*\s*\(') {
        Write-VpsCiEvent -Gate $gate -Level error -Status failed `
            -FailureClass 'libpq_call_outside_adapter' `
            -RelativePath (Get-VpsRelativePath -Root $rootPath -Path $file.FullName)
        exit 1
    }
}

$validation = Get-Content -LiteralPath `
    (Join-Path $rootPath 'src/vps_query_validation.c') -Raw
if ($validation -notmatch [regex]::Escape('SELECT * FROM (') -or
    $validation -notmatch [regex]::Escape(') AS vps_validation LIMIT 0')) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed `
        -FailureClass 'wrapper_contract_missing' `
        -RelativePath 'src/vps_query_validation.c'
    exit 1
}

$boundary = Get-Content -LiteralPath `
    (Join-Path $rootPath 'src/vps_query_boundary.h') -Raw
foreach ($token in @('BEGIN READ ONLY', 'pg_catalog.set_config', 'COMMIT', 'ROLLBACK')) {
    if ($boundary -notmatch [regex]::Escape($token)) {
        Write-VpsCiEvent -Gate $gate -Level error -Status failed `
            -FailureClass 'readonly_boundary_missing' `
            -RelativePath 'src/vps_query_boundary.h' -Detail "token=$token"
        exit 1
    }
}

Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "portable_files=$($portableFiles.Count),required_files=$($required.Count)"
