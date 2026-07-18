[CmdletBinding()]
param(
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$')]
    [string]$Version = '1.0.0',
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [switch]$SkipReproducibleBuild,
    [switch]$SkipAcceptance
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $root 'scripts\ci\vps-ci-common.ps1')
$root = Assert-VpsSafeRoot -Root $root
$gate = 'dist-package'
$started = [Diagnostics.Stopwatch]::StartNew()
$packageName = "VirtualPostgreSQL-$Version-windows"
$buildPackageRoot = Join-Path $root 'build\stage15-package'
$buildArchive = Join-Path $buildPackageRoot "$packageName.zip"
$distRoot = [IO.Path]::GetFullPath((Join-Path $root 'dist'))
$rootPrefix = [IO.Path]::GetFullPath($root).TrimEnd(
    [IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not ($distRoot + [IO.Path]::DirectorySeparatorChar).StartsWith(
        $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw '[dist-package] dist path must remain inside the repository'
}
$distArchive = Join-Path $distRoot "$packageName.zip"

Write-VpsCiEvent -Gate $gate -Level info -Status started `
    -Detail "version=$Version,reproducible_build=$(-not $SkipReproducibleBuild),acceptance=$(-not $SkipAcceptance)"

if (-not $SkipReproducibleBuild) {
    & pwsh -NoProfile -File (Join-Path $root 'scripts\release\run-reproducible-build.ps1') `
        -Root $root -VisualStudioRoot $VisualStudioRoot
    if ($LASTEXITCODE -ne 0) {
        throw '[dist-package] reproducible build failed'
    }
}

& pwsh -NoProfile -File (Join-Path $root 'scripts\release\package-windows.ps1') `
    -Root $root -Version $Version -VisualStudioRoot $VisualStudioRoot
if ($LASTEXITCODE -ne 0) {
    throw '[dist-package] release package generation failed'
}

if (-not $SkipAcceptance) {
    & pwsh -NoProfile -File (Join-Path $root 'scripts\release\verify-windows-1.0.ps1') `
        -Root $root -PackageRoot "build/stage15-package/$packageName"
    if ($LASTEXITCODE -ne 0) {
        throw '[dist-package] Windows 1.0 acceptance failed'
    }
}

if (-not (Test-Path -LiteralPath $buildArchive -PathType Leaf)) {
    throw '[dist-package] verified build archive is missing'
}
New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
$temporaryArchive = Join-Path $distRoot "$packageName.partial"
try {
    if (Test-Path -LiteralPath $temporaryArchive) {
        Remove-Item -LiteralPath $temporaryArchive -Force
    }
    Copy-Item -LiteralPath $buildArchive -Destination $temporaryArchive
    $sourceHash = (Get-FileHash -LiteralPath $buildArchive `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    $temporaryHash = (Get-FileHash -LiteralPath $temporaryArchive `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($sourceHash -ne $temporaryHash) {
        throw '[dist-package] archive digest changed while publishing to dist'
    }
    if (Test-Path -LiteralPath $distArchive) {
        Remove-Item -LiteralPath $distArchive -Force
    }
    Move-Item -LiteralPath $temporaryArchive -Destination $distArchive
} finally {
    if (Test-Path -LiteralPath $temporaryArchive) {
        Remove-Item -LiteralPath $temporaryArchive -Force
    }
}

Add-Type -AssemblyName System.IO.Compression
$zip = [IO.Compression.ZipFile]::OpenRead($distArchive)
try {
    $entries = @($zip.Entries)
    if ($entries.Count -eq 0 -or
        @($entries | Where-Object {
            -not $_.FullName.StartsWith(
                "$packageName/", [StringComparison]::Ordinal)
        }).Count -ne 0) {
        throw '[dist-package] archive root layout is invalid'
    }
} finally {
    $zip.Dispose()
}

$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -RelativePath "dist/$packageName.zip" `
    -Detail "version=$Version,entries=$($entries.Count),sha256=$sourceHash,duration_ms=$($started.ElapsedMilliseconds)"
