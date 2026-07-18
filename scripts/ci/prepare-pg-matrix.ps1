[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$Destination = 'build/stage15-pg-matrix'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')
$rootPath = Assert-VpsSafeRoot -Root $Root
$destinationPath = Join-Path $rootPath $Destination
$downloadPath = Join-Path $destinationPath 'downloads'
New-Item -ItemType Directory -Force -Path $downloadPath | Out-Null
$packages = @(
    @{Major=15; File='postgresql-15.18-2-windows-x64-binaries.zip'; Hash='B583765CFA06210E98D9DA27A08E5B1CB7D268A4F6DB771A6041E110193FE44E'},
    @{Major=16; File='postgresql-16.14-2-windows-x64-binaries.zip'; Hash='8A7F54C1968D5D49BDCD3F66B1291F736C74B8CB6A26E9874771FCC7837DBF38'},
    @{Major=17; File='postgresql-17.10-2-windows-x64-binaries.zip'; Hash='EF9B1E5E23D2E8A83914BA13D9DC536A72210FBA53FD1808FF1F7E06BB22B106'},
    @{Major=18; File='postgresql-18.4-2-windows-x64-binaries.zip'; Hash='02E239529ED7833D169F98D915D3FEFFE0813264B08B3AE353E78E8B9C97E1A6'}
)
foreach ($package in $packages) {
    $archive = Join-Path $downloadPath $package.File
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        $url = "https://get.enterprisedb.com/postgresql/$($package.File)"
        & curl.exe -L --fail --retry 3 -o $archive $url
        if ($LASTEXITCODE -ne 0) { throw "[pg-matrix] download failed: PG$($package.Major)" }
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $package.Hash) {
        throw "[pg-matrix] archive digest mismatch: PG$($package.Major)"
    }
    $packageRoot = Join-Path $destinationPath "pg$($package.Major)"
    $postgres = Join-Path $packageRoot 'pgsql\bin\postgres.exe'
    if (-not (Test-Path -LiteralPath $postgres -PathType Leaf)) {
        Expand-Archive -LiteralPath $archive -DestinationPath $packageRoot -Force
    }
    if (-not (Test-Path -LiteralPath $postgres -PathType Leaf)) {
        throw "[pg-matrix] extracted server missing: PG$($package.Major)"
    }
    Write-VpsCiEvent -Gate 'pg-matrix-prepare' -Level info -Status passed `
        -Detail "major=$($package.Major),archive=$($package.File),sha256=verified"
}
