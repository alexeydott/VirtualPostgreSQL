[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [string]$PvsStudioPath = 'D:\tools\PVS-Studio\PVS-Studio_Cmd.exe',
    [ValidateSet('All', 'x86', 'x64')][string]$Architecture = 'All'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'static-analysis'
$rootPath = Assert-VpsSafeRoot -Root $Root
$pvsPath = [IO.Path]::GetFullPath($PvsStudioPath)
if (-not (Test-Path -LiteralPath $pvsPath -PathType Leaf)) {
    throw '[static-analysis] PVS-Studio_Cmd.exe is missing'
}

$architectures = @(if ($Architecture -eq 'All') { 'x86'; 'x64' } else { $Architecture })
$before = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw '[static-analysis] unable to read source state' }
$started = [Diagnostics.Stopwatch]::StartNew()

foreach ($preset in @('msvc-x86-static-analysis', 'msvc-x64-static-analysis',
                       'clang-tidy-x64-static-analysis')) {
    if ($preset -like '*x86*' -and $Architecture -eq 'x64') { continue }
    if ($preset -like '*x64*' -and $Architecture -eq 'x86') { continue }
    & pwsh -NoProfile -File (Join-Path $rootPath 'scripts\build-stage1.ps1') `
        -Preset $preset -VisualStudioRoot $VisualStudioRoot
    if ($LASTEXITCODE -ne 0) { throw "[static-analysis] preset failed: $preset" }
}

$pvsVersion = @(& $pvsPath --version 2>&1 | Select-Object -First 1) -join ''
foreach ($arch in $architectures) {
    $cmakeArch = if ($arch -eq 'x86') { 'Win32' } else { 'x64' }
    $buildPath = Join-Path $rootPath "build\stage15-pvs-$arch"
    & cmake -S $rootPath -B $buildPath --fresh -G 'Visual Studio 17 2022' `
        -A $cmakeArch -DVPS_BUILD_EXTENSION=ON -DVPS_ENABLE_WINCRED=ON `
        -DVPS_ENABLE_QUERY_MATERIALIZATION=ON
    if ($LASTEXITCODE -ne 0) { throw "[static-analysis] PVS CMake configure failed: $arch" }
    $solution = Join-Path $buildPath 'VirtualPostgreSQL.sln'
    $report = Join-Path $buildPath "pvs-$arch.plog"
    & $pvsPath -t $solution -p $cmakeArch -c Release -o $report `
        -e $rootPath -E 'vps_embedded_sqlite;ALL_BUILD;ZERO_CHECK' -g
    $pvsExit = $LASTEXITCODE
    $operationalFailure = $pvsExit -band (-bnot (256 -bor 1024))
    if ($operationalFailure -ne 0 -or -not (Test-Path -LiteralPath $report)) {
        throw "[static-analysis] PVS-Studio operational failure: arch=$arch code=$pvsExit"
    }

    [xml]$plog = Get-Content -LiteralPath $report -Raw
    $projectFindings = @($plog.NewDataSet.'PVS-Studio_Analysis_Log' | Where-Object {
        $_.File -match '^\|\?\|\\(?:src|tests)\\'
    })
    $findings = @($projectFindings | Where-Object { [int]$_.Level -eq 1 })
    $advisories = @($projectFindings | Where-Object { [int]$_.Level -eq 2 })
    foreach ($finding in $findings) {
        $relative = ([string]$finding.File).Replace('|?|\', '').Replace('\', '/')
        Write-VpsCiEvent -Gate $gate -Level error -Status failed `
            -FailureClass ([string]$finding.ErrorCode) -RelativePath $relative `
            -Detail "arch=$arch,line=$($finding.Line),severity=$($finding.Level)"
    }
    if ($findings.Count -ne 0) {
        throw "[static-analysis] PVS-Studio findings remain: arch=$arch count=$($findings.Count)"
    }
    Write-VpsCiEvent -Gate $gate -Level info -Status passed `
        -Detail "tool=pvs-studio,arch=$arch,level2_reviewed=$($advisories.Count)"
    Write-VpsCiEvent -Gate $gate -Level info -Status passed `
        -Detail "tool=pvs-studio,version=$pvsVersion,arch=$arch,findings=0"
}

$after = @(& git -C $rootPath status --porcelain=v1 --untracked-files=all)
$sourceChanges = @(Compare-Object -ReferenceObject $before -DifferenceObject $after)
if ($sourceChanges.Count -ne 0) {
    throw '[static-analysis] source tree changed during analysis'
}
$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "duration_ms=$($started.ElapsedMilliseconds),architectures=$($architectures.Count),pvs=enabled"
