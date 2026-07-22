[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$Version = '1.0.0',
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [string]$ReproducibleRoot = 'build/stage15-reproducible',
    [string]$OutputRoot = 'build/stage15-package'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $Root 'scripts\ci\vps-ci-common.ps1')
$rootPath = Assert-VpsSafeRoot -Root $Root
$gate = 'package'
$started = [Diagnostics.Stopwatch]::StartNew()
$output = [IO.Path]::GetFullPath((Join-Path $rootPath $OutputRoot))
$safePrefix = [IO.Path]::GetFullPath((Join-Path $rootPath 'build')) +
              [IO.Path]::DirectorySeparatorChar
if (-not $output.StartsWith($safePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw '[package] output must remain below build/'
}
if (Test-Path -LiteralPath $output) {
    Remove-Item -LiteralPath $output -Recurse -Force
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$packageName = "VirtualPostgreSQL-$Version-windows"
$stage = Join-Path $output $packageName
$zipPath = Join-Path $output "$packageName.zip"
$repro = Join-Path $rootPath $ReproducibleRoot
$reproManifestPath = Join-Path $repro 'reproducibility-manifest.json'
if (-not (Test-Path -LiteralPath $reproManifestPath -PathType Leaf)) {
    throw '[package] reproducibility manifest missing'
}
$reproManifest = Get-Content -LiteralPath $reproManifestPath -Raw | ConvertFrom-Json

function Copy-PackageFile {
    param([Parameter(Mandatory)][string]$Source,
          [Parameter(Mandatory)][string]$Relative)
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "[package] input missing: $Relative"
    }
    $target = Join-Path $stage $Relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $target
}

function Assert-PackageLayout {
    $forbidden = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
        $_.Name -match '(?i)(~|\.tmp|\.pdb|\.lib|\.exp|\.obj|\.exe|\.ilk)$' -or
        $_.FullName -match '(?i)[\\/](__pycache__|CMakeFiles|Testing)[\\/]'
    })
    $dlls = @(Get-ChildItem -LiteralPath $stage -Recurse -Filter '*.dll' -File)
    if ($forbidden.Count -ne 0 -or $dlls.Count -ne 2 -or
        @($dlls | Where-Object Name -ne 'virtualpostgresql.dll').Count -ne 0) {
        throw '[package] forbidden or unexpected runtime artifact'
    }
}

foreach ($directory in @('bin\win32','bin\x64','include\virtualpostgresql',
                          'docs','examples','licenses','sbom','provenance')) {
    New-Item -ItemType Directory -Path (Join-Path $stage $directory) -Force | Out-Null
}
Copy-PackageFile (Join-Path $repro 'x86-pass1\virtualpostgresql.dll') 'bin\win32\virtualpostgresql.dll'
Copy-PackageFile (Join-Path $repro 'x64-pass1\virtualpostgresql.dll') 'bin\x64\virtualpostgresql.dll'
Copy-PackageFile (Join-Path $rootPath 'include\virtualpostgresql\vps_api.h') 'include\virtualpostgresql\vps_api.h'
Copy-PackageFile (Join-Path $rootPath 'README.md') 'docs\README.md'
$docMap = [ordered]@{
    'security.md'='SECURITY.md'; 'type-mapping.md'='TYPE_MAPPING.md';
    'query-sources.md'='QUERY_SOURCE.md';
    'transactions-savepoints.md'='TRANSACTIONS.md'; 'spatial.md'='SPATIAL.md';
    'building.md'='BUILDING.md'; 'provider-abi.md'='PROVIDER_ABI.md';
    'metadata-functions-cache.md'='METADATA_CACHE.md';
    'troubleshooting.md'='TROUBLESHOOTING.md';
    'platform-support.md'='PLATFORM_SUPPORT.md';
    'release-notes-current.md'='RELEASE_NOTES.md';
    'windows-current-acceptance.md'='WINDOWS_ACCEPTANCE.md'
}
foreach ($entry in $docMap.GetEnumerator()) {
    Copy-PackageFile (Join-Path $rootPath "docs\$($entry.Key)") "docs\$($entry.Value)"
}
Get-ChildItem -LiteralPath (Join-Path $rootPath 'examples') -File | ForEach-Object {
    Copy-PackageFile $_.FullName "examples\$($_.Name)"
}
Copy-PackageFile (Join-Path $rootPath 'LICENSE') 'licenses\VirtualPostgreSQL-LICENSE.txt'
Copy-PackageFile (Join-Path $rootPath 'licenses\README.md') 'licenses\README.md'
$archives = @(
    @{Archive='postgresql-18.4.tar.bz2'; Member='postgresql-18.4/COPYRIGHT'; Output='PostgreSQL-COPYRIGHT.txt'},
    @{Archive='openssl-3.5.7.tar.gz'; Member='openssl-3.5.7/LICENSE.txt'; Output='OpenSSL-LICENSE.txt'},
    @{Archive='zlib-1.3.2.tar.gz'; Member='zlib-1.3.2/LICENSE'; Output='zlib-LICENSE.txt'}
)
foreach ($license in $archives) {
    $archive = Join-Path $rootPath "build\downloads\$($license.Archive)"
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        throw "[package] dependency archive missing: $($license.Archive)"
    }
    $text = @(& tar -xOf $archive $license.Member)
    if ($LASTEXITCODE -ne 0 -or $text.Count -eq 0) {
        throw "[package] unable to extract license: $($license.Archive)"
    }
    $text | Set-Content -LiteralPath (Join-Path $stage "licenses\$($license.Output)") -Encoding utf8
}
(Get-Content -LiteralPath (Join-Path $rootPath 'build\deps\sqlite-3.53.3\sqlite-amalgamation-3530300\sqlite3.h') -First 11) |
    Set-Content -LiteralPath (Join-Path $stage 'licenses\SQLite-Public-Domain.txt') -Encoding utf8

foreach ($binary in @(@{Arch='x86';Path='bin\win32\virtualpostgresql.dll'},
                       @{Arch='x64';Path='bin\x64\virtualpostgresql.dll'})) {
    & pwsh -NoProfile -File (Join-Path $rootPath 'scripts\ci\test-windows-binary.ps1') `
        -DllPath (Join-Path $stage $binary.Path) -Architecture $binary.Arch `
        -VisualStudioRoot $VisualStudioRoot
    if ($LASTEXITCODE -ne 0) { throw "[package] PE inspection failed: $($binary.Arch)" }
}

$versions = Get-Content -LiteralPath (Join-Path $rootPath 'deps\versions.json') -Raw | ConvertFrom-Json
$components = [Collections.Generic.List[object]]::new()
$components.Add([ordered]@{type='library'; name='VirtualPostgreSQL'; version=$Version;
    licenses=@([ordered]@{license=[ordered]@{id='MIT'}}); purl="pkg:generic/virtualpostgresql@$Version"})
foreach ($dependency in $versions.dependencies) {
    $components.Add([ordered]@{type='library'; name=$dependency.name;
        version=$dependency.version;
        hashes=@([ordered]@{alg='SHA-256';content=$dependency.sha256});
        licenses=@([ordered]@{license=[ordered]@{name=$dependency.license}})})
}
$sbomSerialBytes = [Security.Cryptography.SHA256]::HashData(
    [Text.Encoding]::UTF8.GetBytes([string]$reproManifest.source_tree))
$sbomGuidBytes = [byte[]]::new(16)
[Array]::Copy($sbomSerialBytes, $sbomGuidBytes, 16)
$sbom = [ordered]@{
    bomFormat='CycloneDX'; specVersion='1.6'; serialNumber="urn:uuid:$([guid]::new($sbomGuidBytes))";
    version=1; metadata=[ordered]@{component=$components[0]};
    components=@($components | Select-Object -Skip 1)
}

function Assert-PackageMarkdownEnglish {
    foreach ($markdown in Get-ChildItem -LiteralPath $stage -Recurse -File -Filter '*.md') {
        $content = Get-Content -LiteralPath $markdown.FullName -Raw
        if ($content -match '[А-Яа-яЁё]') {
            $relative = [IO.Path]::GetRelativePath($stage, $markdown.FullName)
            throw "[package] non-English Markdown content found: $relative"
        }
    }
}
$sbom | ConvertTo-Json -Depth 10 | Set-Content `
    -LiteralPath (Join-Path $stage 'sbom\cyclonedx.json') -Encoding utf8

$subjects = @('bin\win32\virtualpostgresql.dll','bin\x64\virtualpostgresql.dll') | ForEach-Object {
    $digest = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage $_)).Hash.ToLowerInvariant()
    [ordered]@{name=$_.Replace('\','/'); digest=[ordered]@{sha256=$digest}}
}
$provenance = [ordered]@{
    _type='https://in-toto.io/Statement/v1'; subject=$subjects;
    predicateType='https://slsa.dev/provenance/v1';
    predicate=[ordered]@{
        buildDefinition=[ordered]@{
            buildType='https://virtualpostgresql.dev/build/windows-1.0';
            externalParameters=[ordered]@{architectures=@('x86','x64');configuration='Release'};
            internalParameters=[ordered]@{reproducible=$true};
            resolvedDependencies=@(
                [ordered]@{uri='git+local:source';digest=[ordered]@{gitTree=$reproManifest.source_tree}},
                [ordered]@{uri='file:deps/versions.json';digest=[ordered]@{sha256=$reproManifest.dependency_manifest_sha256}}
            )
        };
        runDetails=[ordered]@{
            builder=[ordered]@{id='https://virtualpostgresql.dev/builders/msvc-2022'};
            metadata=[ordered]@{invocationId=$reproManifest.source_tree}
        }
    }
}
$provenance | ConvertTo-Json -Depth 12 | Set-Content `
    -LiteralPath (Join-Path $stage 'provenance\slsa-provenance.json') -Encoding utf8

Assert-PackageMarkdownEnglish

$payloadFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)
$manifestFiles = @($payloadFiles | ForEach-Object {
    [ordered]@{path=[IO.Path]::GetRelativePath($stage,$_.FullName).Replace('\','/');
        size=$_.Length; sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
})
$manifest = [ordered]@{schema_version=1;version=$Version;platform='windows';
    source_tree=$reproManifest.source_tree; architectures=@('win32','x64');files=$manifestFiles}
$manifest | ConvertTo-Json -Depth 8 | Set-Content `
    -LiteralPath (Join-Path $stage 'manifest.json') -Encoding utf8
$checksumFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)
$checksumFiles | ForEach-Object {
    $relative = [IO.Path]::GetRelativePath($stage,$_.FullName).Replace('\','/')
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
    "$hash  $relative"
} | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS') -Encoding ascii

Assert-PackageLayout
$negative = Join-Path $stage 'forbidden-test.pdb'
'negative fixture' | Set-Content -LiteralPath $negative -Encoding ascii
$negativeRejected = $false
try { Assert-PackageLayout } catch { $negativeRejected = $true }
Remove-Item -LiteralPath $negative -Force
if (-not $negativeRejected) { throw '[package] forbidden injection was accepted' }

$epoch = [DateTimeOffset]::FromUnixTimeSeconds([int64]$reproManifest.source_date_epoch)
Get-ChildItem -LiteralPath $stage -Recurse -Force | ForEach-Object {
    $_.LastWriteTimeUtc = $epoch.UtcDateTime
}
Add-Type -AssemblyName System.IO.Compression
$archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName) {
        $relative = "$packageName/" + [IO.Path]::GetRelativePath($stage,$file.FullName).Replace('\','/')
        $entry = $archive.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = $epoch
        $input = [IO.File]::OpenRead($file.FullName)
        $stream = $entry.Open()
        try { $input.CopyTo($stream) } finally { $stream.Dispose(); $input.Dispose() }
    }
} finally { $archive.Dispose() }
$zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "layout=passed,architectures=2,exports=verified,imports=verified,licenses=5,sbom=cyclonedx-1.6,provenance=slsa-v1,manifest_files=$($manifestFiles.Count),forbidden_injection=rejected,zip_sha256=$zipHash"
$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "duration_ms=$($started.ElapsedMilliseconds),artifact=build/stage15-package/$packageName.zip"
