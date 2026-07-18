[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [ValidateRange(1000000, 100000000)][int]$Runs = 1000000,
    [ValidateRange(30, 3600)][int]$TargetTimeoutSeconds = 300
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')
$rootPath = Assert-VpsSafeRoot -Root $Root
$gate = 'fuzz'
$started = [Diagnostics.Stopwatch]::StartNew()

& pwsh -NoProfile -File (Join-Path $rootPath 'scripts\build-stage1.ps1') `
    -Preset clang-cl-x64-asan -VisualStudioRoot $VisualStudioRoot
if ($LASTEXITCODE -ne 0) { throw '[fuzz] ASan fuzz build failed' }

$buildPath = Join-Path $rootPath 'build\stage2-clang-cl-x64-asan'
$corpusRoot = Join-Path $buildPath 'stage15-fuzz-corpora'
$logRoot = Join-Path $buildPath 'stage15-fuzz-logs'
New-Item -ItemType Directory -Force -Path $corpusRoot, $logRoot | Out-Null
$targets = @(
    @{ Name='vps_arguments_fuzz'; Seeds='vps_arguments_seed_*'; Max=8192; Dict=$null },
    @{ Name='vps_conninfo_fuzz'; Seeds='vps_conninfo_seed_*'; Max=8192; Dict=$null },
    @{ Name='vps_query_source_fuzz'; Seeds='vps_query_source_seed_*'; Max=8192; Dict='vps_sql.dict' },
    @{ Name='vps_query_indexes_fuzz'; Seeds='vps_query_indexes_seed_*'; Max=8192; Dict='vps_sql.dict' },
    @{ Name='vps_query_metadata_fuzz'; Seeds='vps_query_metadata_seed_*'; Max=8192; Dict='vps_sql.dict' },
    @{ Name='vps_planner_fuzz'; Seeds='vps_planner_seed_*'; Max=6552; Dict=$null },
    @{ Name='vps_row_identity_fuzz'; Seeds='vps_row_identity_seed_*'; Max=65536; Dict=$null },
    @{ Name='vps_type_codec_fuzz'; Seeds='vps_type_codec_seed_*'; Max=65536; Dict=$null },
    @{ Name='vps_spatial_fuzz'; Seeds='vps_spatial_seed_*'; Max=65536; Dict='vps_spatial.dict' },
    @{ Name='vps_redaction_fuzz'; Seeds='vps_redaction_seed_*'; Max=4096; Dict=$null }
)

foreach ($target in $targets) {
    $executable = Join-Path $buildPath "$($target.Name).exe"
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "[fuzz] target is missing: $($target.Name)"
    }
    $corpus = Join-Path $corpusRoot $target.Name
    New-Item -ItemType Directory -Force -Path $corpus | Out-Null
    Get-ChildItem -Path (Join-Path $rootPath "tests\fuzz\$($target.Seeds)") -File |
        Copy-Item -Destination $corpus -Force
    if (@(Get-ChildItem -LiteralPath $corpus -File).Count -eq 0) {
        throw "[fuzz] seed corpus is empty: $($target.Name)"
    }
    $arguments = @($corpus, "-runs=$Runs", "-max_len=$($target.Max)",
                   '-timeout=5', '-rss_limit_mb=1024', '-print_final_stats=1')
    if ($null -ne $target.Dict) {
        $arguments += "-dict=$(Join-Path $rootPath "tests\fuzz\$($target.Dict)")"
    }
    $stdout = Join-Path $logRoot "$($target.Name).out.log"
    $stderr = Join-Path $logRoot "$($target.Name).err.log"
    $process = Start-Process -FilePath $executable -ArgumentList $arguments `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr
    if (-not $process.WaitForExit($TargetTimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -Force
        throw "[fuzz] target timeout: $($target.Name)"
    }
    if ($process.ExitCode -ne 0) {
        Get-Content -LiteralPath $stderr -Tail 40
        throw "[fuzz] target failed: $($target.Name) code=$($process.ExitCode)"
    }
    $evidence = (Get-Content -LiteralPath $stderr -Raw) +
                (Get-Content -LiteralPath $stdout -Raw)
    $matches = [regex]::Matches($evidence, 'number_of_executed_units:\s+(\d+)')
    if ($matches.Count -eq 0 -or
        [int64]$matches[$matches.Count - 1].Groups[1].Value -lt $Runs) {
        throw "[fuzz] insufficient iteration evidence: $($target.Name)"
    }
    Write-VpsCiEvent -Gate $gate -Level info -Status passed `
        -Detail "target=$($target.Name),runs=$Runs,max_len=$($target.Max)"
}
$started.Stop()
Write-VpsCiEvent -Gate $gate -Level info -Status passed `
    -Detail "duration_ms=$($started.ElapsedMilliseconds),targets=$($targets.Count),runs_each=$Runs"
