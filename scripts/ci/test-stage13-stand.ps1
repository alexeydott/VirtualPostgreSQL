[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$BuildDirectory = 'build/stage1-msvc-x64-release',
    [string]$Psql = 'D:\tools\pgAdmin4\runtime\psql.exe'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$required = @('VPS_SESSION_TEST_HOST', 'VPS_SESSION_TEST_PORT',
              'VPS_SESSION_TEST_USER', 'VPS_SESSION_TEST_PASSWORD',
              'VPS_SESSION_TEST_DBNAME')
foreach ($name in $required) {
    if ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name))) {
        throw "[stage13-stand] required runtime environment variable is missing: $name"
    }
}
if (!(Test-Path -LiteralPath $Psql -PathType Leaf)) {
    throw '[stage13-stand] psql executable is missing'
}

$temporary = @(
    'PGPASSWORD', 'VPS_ASYNC_TEST_HOST', 'VPS_ASYNC_TEST_PORT',
    'VPS_ASYNC_TEST_USER', 'VPS_ASYNC_TEST_PASSWORD', 'VPS_ASYNC_TEST_DBNAME',
    'VPS_ASYNC_TEST_SSLMODE', 'VPS_ASYNC_TEST_CHANNEL_BINDING',
    'VPS_VTAB_TEST_SPATIAL'
)
$saved = @{}
foreach ($name in $temporary) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
    $env:PGPASSWORD = $env:VPS_SESSION_TEST_PASSWORD
    $fixtureSql = @'
CREATE EXTENSION IF NOT EXISTS postgis;
DROP TABLE IF EXISTS public.vps_stage13_spatial;
CREATE TABLE public.vps_stage13_spatial (
  id bigint PRIMARY KEY,
  label text NOT NULL,
  geom geometry(PointZ,4326) NOT NULL,
  geog geography(Point,4326) NOT NULL,
  nullable_geom geometry,
  empty_geom geometry
);
INSERT INTO public.vps_stage13_spatial
  (id,label,geom,geog,nullable_geom,empty_geom)
VALUES
  (1,'control',
   ST_GeomFromEWKT('SRID=4326;POINT Z (30 10 5)'),
   ST_GeogFromText('SRID=4326;POINT(-71.060316 48.432044)'),
   NULL,ST_GeomFromText('GEOMETRYCOLLECTION EMPTY',4326));
CREATE INDEX vps_stage13_spatial_geom_gix
  ON public.vps_stage13_spatial USING gist (geom);
CREATE INDEX vps_stage13_spatial_geog_gix
  ON public.vps_stage13_spatial USING gist (geog);
'@
    & $Psql -X -v ON_ERROR_STOP=1 `
        -h $env:VPS_SESSION_TEST_HOST -p $env:VPS_SESSION_TEST_PORT `
        -U $env:VPS_SESSION_TEST_USER -d $env:VPS_SESSION_TEST_DBNAME `
        -c $fixtureSql
    if ($LASTEXITCODE -ne 0) { throw '[stage13-stand] fixture bootstrap failed' }
    foreach ($suffix in @('HOST', 'PORT', 'USER', 'PASSWORD', 'DBNAME')) {
        [Environment]::SetEnvironmentVariable(
            "VPS_ASYNC_TEST_$suffix",
            [Environment]::GetEnvironmentVariable("VPS_SESSION_TEST_$suffix"))
    }
    $env:VPS_ASYNC_TEST_SSLMODE = 'disable'
    $env:VPS_ASYNC_TEST_CHANNEL_BINDING = 'disable'
    $env:VPS_VTAB_TEST_SPATIAL = '1'
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-vtable-stand.ps1') `
        -Root $Root -BuildDirectory $BuildDirectory
    if ($LASTEXITCODE -ne 0) { throw '[stage13-stand] spatial contour failed' }
    $env:VPS_VTAB_TEST_SPATIAL = $null
    $env:VPS_ASYNC_TEST_DBNAME = 'postgres'
    $postgisCount = (& $Psql -X -At -v ON_ERROR_STOP=1 `
        -h $env:VPS_SESSION_TEST_HOST -p $env:VPS_SESSION_TEST_PORT `
        -U $env:VPS_SESSION_TEST_USER -d postgres `
        -c "SELECT count(*) FROM pg_catalog.pg_extension WHERE extname='postgis'").Trim()
    if ($LASTEXITCODE -ne 0 -or $postgisCount -ne '0') {
        throw '[stage13-stand] postgres database is not an absent-extension contour'
    }
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test-vtable-stand.ps1') `
        -Root $Root -BuildDirectory $BuildDirectory
    if ($LASTEXITCODE -ne 0) { throw '[stage13-stand] absent-extension contour failed' }
} finally {
    foreach ($name in $temporary) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name])
    }
}
Write-Output 'stage13_stand status=passed formats=wkt,wkb,ewkt,ewkb kinds=geometry,geography malformed=local spatialite=unavailable'
