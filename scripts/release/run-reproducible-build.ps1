[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [string]$OutputRoot = 'build/stage15-reproducible'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $Root 'scripts\ci\vps-ci-common.ps1')
$rootPath = Assert-VpsSafeRoot -Root $Root
$output = [IO.Path]::GetFullPath((Join-Path $rootPath $OutputRoot))
$safePrefix = [IO.Path]::GetFullPath((Join-Path $rootPath 'build')) +
              [IO.Path]::DirectorySeparatorChar
if (-not $output.StartsWith($safePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw '[reproducibility] output must remain below build/'
}
if (Test-Path -LiteralPath $output) {
    Remove-Item -LiteralPath $output -Recurse -Force
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$gate = 'reproducibility'
$started = [Diagnostics.Stopwatch]::StartNew()
$sourceEpoch = (& git -C $rootPath show -s --format=%ct HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $sourceEpoch -notmatch '^\d+$') {
    throw '[reproducibility] unable to resolve source epoch'
}
$savedEpoch = $env:SOURCE_DATE_EPOCH
$temporaryIndex = Join-Path $output 'source.index'
$savedIndex = $env:GIT_INDEX_FILE
try {
    $env:SOURCE_DATE_EPOCH = $sourceEpoch
    $env:GIT_INDEX_FILE = $temporaryIndex
    & git -C $rootPath read-tree HEAD
    if ($LASTEXITCODE -ne 0) { throw '[reproducibility] temporary index init failed' }
    & git -C $rootPath add -A -- .
    if ($LASTEXITCODE -ne 0) { throw '[reproducibility] source snapshot failed' }
    $sourceTree = (& git -C $rootPath write-tree).Trim()
    if ($LASTEXITCODE -ne 0 -or $sourceTree -notmatch '^[0-9a-f]{40}$') {
        throw '[reproducibility] source tree hash failed'
    }
    $dependencyHash = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath (Join-Path $rootPath 'deps\versions.json')).Hash.ToLowerInvariant()
    $results = [Collections.Generic.List[object]]::new()
    foreach ($architecture in @('x86','x64')) {
        $vcvarsName = if ($architecture -eq 'x86') { 'vcvars32.bat' } else { 'vcvars64.bat' }
        $vcvars = Join-Path $VisualStudioRoot "VC\Auxiliary\Build\$vcvarsName"
        if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) {
            throw "[reproducibility] vcvars missing: $architecture"
        }
        $digests = [Collections.Generic.List[string]]::new()
        foreach ($pass in 1,2) {
            $build = Join-Path $output "$architecture-pass$pass"
            $command = "call `"$vcvars`" >nul && cmake -S `"$rootPath`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DVPS_REPRODUCIBLE_BUILD=ON && cmake --build `"$build`" --target virtualpostgresql"
            & cmd.exe /d /s /c $command
            if ($LASTEXITCODE -ne 0) {
                throw "[reproducibility] clean build failed: arch=$architecture pass=$pass"
            }
            $dll = Join-Path $build 'virtualpostgresql.dll'
            if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
                throw '[reproducibility] DLL missing'
            }
            $digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $dll).Hash.ToLowerInvariant()
            $digests.Add($digest)
            $results.Add([ordered]@{architecture=$architecture; pass=$pass; sha256=$digest})
        }
        if ($digests[0] -ne $digests[1]) {
            throw "[reproducibility] byte digest mismatch: arch=$architecture"
        }
        Write-VpsCiEvent -Gate $gate -Level info -Status passed `
            -Detail "architecture=$architecture,builds=2,byte_identical=true,sha256=$($digests[0])"
    }
    $compilerVersion = (& cmd.exe /d /s /c `
        "call `"$(Join-Path $VisualStudioRoot 'VC\Auxiliary\Build\vcvars64.bat')`" >nul && cl 2>&1" | Select-Object -First 1).Trim()
    $manifest = [ordered]@{
        schema_version = 1
        source_tree = $sourceTree
        source_date_epoch = [int64]$sourceEpoch
        dependency_manifest_sha256 = $dependencyHash
        cmake_version = (cmake --version | Select-Object -First 1).Trim()
        ninja_version = (ninja --version).Trim()
        compiler = $compilerVersion
        flags = @('/Brepro', '/experimental:deterministic',
                  "/pathmap:$rootPath=.", '/MT')
        builds = $results
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content `
        -LiteralPath (Join-Path $output 'reproducibility-manifest.json') -Encoding utf8
} finally {
    $env:SOURCE_DATE_EPOCH = $savedEpoch
    $env:GIT_INDEX_FILE = $savedIndex
}
$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "duration_ms=$($started.ElapsedMilliseconds),architectures=2,clean_builds=4,manifest=build/stage15-reproducible/reproducibility-manifest.json"
