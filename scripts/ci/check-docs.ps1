[CmdletBinding()]
param([string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')
$rootPath = Assert-VpsSafeRoot -Root $Root
$required = @(
    'README.md','docs/building.md','docs/security.md','docs/type-mapping.md',
    'docs/query-sources.md','docs/transactions-savepoints.md','docs/spatial.md',
    'docs/metadata-functions-cache.md','docs/provider-abi.md',
    'docs/troubleshooting.md','docs/platform-support.md',
    'examples/read-only.sql','examples/dml-transactions.sql',
    'examples/spatial.sql','examples/credential-provider.c'
)
$navigationOrder = @(
    'building.md','connection-credentials.md','provider-abi.md','security.md',
    'client-runtime.md','table-metadata.md','type-mapping.md','query-sources.md',
    'read-only-vtable.md','planner-pushdown.md','streaming-cancellation.md',
    'dml-identity.md','transactions-savepoints.md','spatial.md',
    'metadata-functions-cache.md','static-analysis.md','sanitizers.md',
    'troubleshooting.md','platform-support.md','release-notes-1.0.0.md',
    'windows-1.0-acceptance.md'
)
$files = [Collections.Generic.List[IO.FileInfo]]::new()
foreach ($relative in $required) {
    $path = Join-Path $rootPath $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "[docs] required artifact missing: $relative"
    }
}
$readme = Get-Content -LiteralPath (Join-Path $rootPath 'README.md')
if ($readme.Count -ge 150 -or ($readme -join "`n") -notmatch '## Quick start' -or
    ($readme -join "`n") -notmatch '## Документация' -or
    ($readme -join "`n") -notmatch '## License') {
    throw '[docs] README landing-page contract failed'
}
Get-ChildItem -LiteralPath (Join-Path $rootPath 'docs') -Filter '*.md' -File |
    ForEach-Object { $files.Add($_) }
$docsFiles = @($files)
$files.Add((Get-Item -LiteralPath (Join-Path $rootPath 'README.md')))
$files.Add((Get-Item -LiteralPath (Join-Path $rootPath 'examples\README.md')))
$linkCount = 0
foreach ($file in $files) {
    $content = Get-Content -LiteralPath $file.FullName -Raw
    if ($file.Directory.Name -eq 'docs') {
        $lines = Get-Content -LiteralPath $file.FullName
        if ($lines.Count -eq 0 -or $lines[0] -notmatch '\[Back to README\]') {
            throw "[docs] navigation missing: $($file.Name)"
        }
        if ($content -notmatch '(?m)^## See Also\s*$') {
            throw "[docs] See Also missing: $($file.Name)"
        }
    }
    foreach ($match in [regex]::Matches($content, '\[[^\]]+\]\(([^)#]+)(?:#[^)]+)?\)')) {
        $target = $match.Groups[1].Value
        if ($target -match '^[a-z]+:' -or $target.StartsWith('#')) { continue }
        $resolved = [IO.Path]::GetFullPath((Join-Path $file.Directory.FullName $target))
        if (-not (Test-Path -LiteralPath $resolved)) {
            throw "[docs] broken link: $($file.Name) -> $target"
        }
        ++$linkCount
    }
}
for ($index = 0; $index -lt $navigationOrder.Count; ++$index) {
    $line = (Get-Content -LiteralPath (Join-Path $rootPath "docs\$($navigationOrder[$index])") -First 1)
    if ($index -eq 0) {
        if ($line -match 'Previous') { throw '[docs] first page has previous navigation' }
    } elseif ($line -notmatch [regex]::Escape($navigationOrder[$index - 1])) {
        throw "[docs] previous navigation mismatch: $($navigationOrder[$index])"
    }
    if ($index -eq $navigationOrder.Count - 1) {
        if ($line -match 'Next:') { throw '[docs] last page has next navigation' }
    } elseif ($line -notmatch [regex]::Escape($navigationOrder[$index + 1])) {
        throw "[docs] next navigation mismatch: $($navigationOrder[$index])"
    }
}
foreach ($file in $docsFiles) {
    if ((Get-Content -LiteralPath $file.FullName -Raw) -match '[А-Яа-яЁё]') {
        throw "[docs] non-English Cyrillic text found: $($file.Name)"
    }
}
$publicHeaderPath = Join-Path $rootPath 'include\virtualpostgresql\vps_api.h'
$publicHeader = Get-Content -LiteralPath $publicHeaderPath
$publicDeclarationCount = 0
for ($index = 0; $index -lt $publicHeader.Count; ++$index) {
    $line = $publicHeader[$index]
    $isPublicType = $line -match '^typedef (?:uint64_t VpsCredentialFields;|struct Vps(?:AbiHeader|CredentialConfig|CredentialLease|CredentialProvider)\s*\{|(?:int32_t|void)\(VPS_CALL \*VpsCredential(?:Resolve|Release)Fn\)\()'
    $isPublicFunction = $line -match '^VPS_API\s+'
    if (-not $isPublicType -and -not $isPublicFunction) { continue }
    ++$publicDeclarationCount
    $previous = $index - 1
    while ($previous -ge 0 -and $publicHeader[$previous].Trim().Length -eq 0) {
        --$previous
    }
    if ($previous -lt 0 -or $publicHeader[$previous].Trim() -notmatch '\*/$') {
        throw "[docs] public API declaration lacks an adjacent comment: line=$($index + 1)"
    }
}
if ($publicDeclarationCount -ne 14) {
    throw "[docs] public API declaration inventory changed: count=$publicDeclarationCount"
}
$allText = @($files | ForEach-Object {
    Get-Content -LiteralPath $_.FullName -Raw
}) -join "`n"
if ($allText -match '(?i)(password|access[_ -]?token)\s*=\s*[^\[<\s]') {
    throw '[docs] secret-like assignment found'
}
if ($allText -notmatch 'stock\s+Android\s+SQLite' -or
    $allText -notmatch 'static entry-point registration') {
    throw '[docs] Android host SQLite caveat missing'
}
Write-VpsCiEvent -Gate 'docs' -Level info -Status passed `
    -Detail "files=$($files.Count),links=$linkCount,broken=0,navigation_pages=$($navigationOrder.Count),english_docs=$($docsFiles.Count),public_api_comments=$publicDeclarationCount,readme_lines=$($readme.Count),required_examples=4,android_caveat=present"
