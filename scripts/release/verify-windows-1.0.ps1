[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$PackageRoot = 'build/stage15-package/VirtualPostgreSQL-1.0.0-windows'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $Root 'scripts\ci\vps-ci-common.ps1')
$rootPath = Assert-VpsSafeRoot -Root $Root
$gate = 'windows-1.0-acceptance'
$matrixPath = Join-Path $rootPath 'docs\windows-1.0-acceptance.md'
$package = Join-Path $rootPath $PackageRoot
$reproManifestPath = Join-Path $rootPath 'build\stage15-reproducible\reproducibility-manifest.json'

& pwsh -NoProfile -File (Join-Path $rootPath 'scripts\ci\check-docs.ps1')
if ($LASTEXITCODE -ne 0) { throw '[acceptance] documentation gate failed' }
& pwsh -NoProfile -File (Join-Path $rootPath 'scripts\ci\test-source-tree.ps1') -Root $rootPath
if ($LASTEXITCODE -ne 0) { throw '[acceptance] source boundary gate failed' }

$matrix = Get-Content -LiteralPath $matrixPath
$rows = @($matrix | Where-Object { $_ -match '^\|\s*(\d+)\s*\|' })
if ($rows.Count -ne 132) { throw '[acceptance] matrix must contain 132 criteria' }
for ($index = 0; $index -lt $rows.Count; ++$index) {
    if ($rows[$index] -notmatch '^\|\s*(\d+)\s*\|.*\|\s*PASS\s*\|$' -or
        [int]$Matches[1] -ne $index + 1) {
        throw "[acceptance] criterion sequence/status invalid: expected=$($index + 1)"
    }
}

$header = Get-Content -LiteralPath (Join-Path $rootPath 'include\virtualpostgresql\vps_api.h') -Raw
$cmake = Get-Content -LiteralPath (Join-Path $rootPath 'CMakeLists.txt') -Raw
$resource = Get-Content -LiteralPath (Join-Path $rootPath 'src\virtualpostgresql.rc') -Raw
if ($header -notmatch 'VPS_API_VERSION_MAJOR\s+UINT32_C\(1\)' -or
    $header -notmatch 'VPS_API_VERSION_MINOR\s+UINT32_C\(0\)' -or
    $header -notmatch 'VPS_API_VERSION_PATCH\s+UINT32_C\(0\)' -or
    $cmake -notmatch '(?s)project\(\s*VirtualPostgreSQL\s+VERSION 1\.0\.0' -or
    $resource -notmatch 'FILEVERSION 1,0,0,0' -or
    $resource -notmatch 'VALUE "ProductVersion", "1\.0\.0\\0"') {
    throw '[acceptance] version freeze mismatch'
}
$expectedExports = @(Get-Content -LiteralPath (Join-Path $rootPath 'scripts\ci\expected-exports.txt') |
    ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
$definitionExports = @(Get-Content -LiteralPath (Join-Path $rootPath 'src\virtualpostgresql.def') |
    Select-Object -Skip 2 | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
if ($expectedExports.Count -ne 6 -or
    @(Compare-Object $expectedExports $definitionExports).Count -ne 0) {
    throw '[acceptance] public export freeze mismatch'
}

foreach ($abi in @(
    @{Arch='x86';Exe='build/stage1-msvc-x86-debug/vps_abi_layout_test.exe';Pattern='pointer_bits=32 config_size=112 lease_size=40 provider_size=48 status=passed'},
    @{Arch='x64';Exe='build/stage1-msvc-x64-release/vps_abi_layout_test.exe';Pattern='pointer_bits=64 config_size=200 lease_size=64 provider_size=72 status=passed'})) {
    $output = @(& (Join-Path $rootPath $abi.Exe) 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $output -notmatch [regex]::Escape($abi.Pattern)) {
        throw "[acceptance] ABI layout failed: $($abi.Arch)"
    }
}

$requiredPackage = @(
    'bin\win32\virtualpostgresql.dll','bin\x64\virtualpostgresql.dll',
    'include\virtualpostgresql\vps_api.h','docs\README.md',
    'docs\TECHNICAL_SPECIFICATION.md','docs\SECURITY.md','docs\TYPE_MAPPING.md',
    'docs\QUERY_SOURCE.md','docs\TRANSACTIONS.md','docs\SPATIAL.md',
    'docs\BUILDING.md','examples\read-only.sql','licenses\OpenSSL-LICENSE.txt',
    'sbom\cyclonedx.json','provenance\slsa-provenance.json','manifest.json','SHA256SUMS'
)
foreach ($relative in $requiredPackage) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $relative) -PathType Leaf)) {
        throw "[acceptance] package artifact missing: $relative"
    }
}
$sbom = Get-Content -LiteralPath (Join-Path $package 'sbom\cyclonedx.json') -Raw | ConvertFrom-Json
$provenance = Get-Content -LiteralPath (Join-Path $package 'provenance\slsa-provenance.json') -Raw | ConvertFrom-Json
if ($sbom.bomFormat -ne 'CycloneDX' -or $sbom.specVersion -ne '1.6' -or
    @($sbom.components).Count -ne 4 -or
    $provenance.predicateType -ne 'https://slsa.dev/provenance/v1' -or
    @($provenance.subject).Count -ne 2) {
    throw '[acceptance] SBOM/provenance contract failed'
}
$reproManifest = Get-Content -LiteralPath $reproManifestPath -Raw | ConvertFrom-Json
$matrixHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $matrixPath).Hash.ToLowerInvariant()
$attestation = [ordered]@{
    schema_version=1; product='VirtualPostgreSQL'; version='1.0.0';
    criteria_total=132; criteria_passed=132; criteria_failed=0;
    matrix_sha256=$matrixHash; source_tree=$reproManifest.source_tree;
    verifier='automated-windows-1.0-gate'; status='accepted'
}
$evidence = Join-Path $rootPath 'build\stage15-acceptance'
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$attestation | ConvertTo-Json -Depth 4 | Set-Content `
    -LiteralPath (Join-Path $evidence 'windows-1.0-attestation.json') -Encoding utf8
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "api=1.0.0,abi_architectures=2,exports=6,criteria=132,passed=132,failed=0,matrix_sha256=$matrixHash,source_tree=$($reproManifest.source_tree)"
