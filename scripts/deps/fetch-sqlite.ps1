[CmdletBinding()]
param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot '..\..\deps\versions.json'),
    [string]$DownloadsRoot = (Join-Path $PSScriptRoot '..\..\build\downloads'),
    [string]$BuildRoot = (Join-Path $PSScriptRoot '..\..\build\deps'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$sqlite = $manifest.dependencies | Where-Object { $_.name -eq 'SQLite' }
if ($null -eq $sqlite) {
    throw '[sqlite-fetch] dependency entry not found'
}

$dependencyRoot = Join-Path $BuildRoot "sqlite-$($sqlite.version)"
$archivePath = Join-Path $DownloadsRoot $sqlite.archive
$extractRoot = Join-Path $dependencyRoot ([IO.Path]::GetFileNameWithoutExtension($sqlite.archive))
New-Item -ItemType Directory -Path $dependencyRoot -Force | Out-Null
New-Item -ItemType Directory -Path $DownloadsRoot -Force | Out-Null

if ($Force -or -not (Test-Path -LiteralPath $archivePath)) {
    Write-Information "[sqlite-fetch] phase=download version=$($sqlite.version) status=start" -InformationAction Continue
    Invoke-WebRequest -Uri $sqlite.url -OutFile $archivePath
}

$actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $sqlite.sha256) {
    throw '[sqlite-fetch] archive SHA-256 mismatch'
}
Write-Information "[sqlite-fetch] phase=verify version=$($sqlite.version) algorithm=sha256 status=passed" -InformationAction Continue

if ($Force -or -not (Test-Path -LiteralPath (Join-Path $extractRoot 'sqlite3ext.h'))) {
    Expand-Archive -LiteralPath $archivePath -DestinationPath $dependencyRoot -Force
}

foreach ($requiredFile in @('sqlite3.c', 'sqlite3.h', 'sqlite3ext.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $extractRoot $requiredFile))) {
        throw "[sqlite-fetch] required file missing: $requiredFile"
    }
}
Write-Information "[sqlite-fetch] phase=extract version=$($sqlite.version) status=passed" -InformationAction Continue
