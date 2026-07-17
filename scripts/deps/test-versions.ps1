[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repository_root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$validator = Join-Path $PSScriptRoot 'versions.ps1'
$manifest_path = Join-Path $repository_root 'deps\versions.json'
$downloads_root = Join-Path $repository_root 'build\downloads'
$test_root = Join-Path $repository_root 'build\test-versions'
New-Item -ItemType Directory -Force -Path $test_root | Out-Null

function Invoke-VpsValidator {
    param(
        [string]$Case,
        [string[]]$Arguments,
        [int]$ExpectedExitCode
    )

    [Console]::WriteLine("[vps] level=info event=test_start case=$Case")
    & pwsh -NoProfile -File $validator @Arguments
    $actual_exit_code = $LASTEXITCODE
    if ($actual_exit_code -ne $ExpectedExitCode) {
        throw "validator case failed: case=$Case expected_exit=$ExpectedExitCode actual_exit=$actual_exit_code"
    }
    [Console]::WriteLine("[vps] level=info event=test_complete case=$Case exit=$actual_exit_code")
}

Invoke-VpsValidator -Case valid_first_run -Arguments @('-ManifestPath', $manifest_path, '-DownloadsRoot', $downloads_root, '-RequireArchives') -ExpectedExitCode 0
Invoke-VpsValidator -Case valid_repeat_run -Arguments @('-ManifestPath', $manifest_path, '-DownloadsRoot', $downloads_root, '-RequireArchives') -ExpectedExitCode 0

$mismatch_manifest_path = Join-Path $test_root 'versions-mismatch.json'
$mismatch_manifest = Get-Content -LiteralPath $manifest_path -Raw | ConvertFrom-Json
$mismatch_manifest.dependencies[0].sha256 = '0000000000000000000000000000000000000000000000000000000000000000'
$mismatch_manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $mismatch_manifest_path -Encoding utf8NoBOM
Invoke-VpsValidator -Case deliberate_hash_mismatch -Arguments @('-ManifestPath', $mismatch_manifest_path, '-DownloadsRoot', $downloads_root, '-RequireArchives', '-SkipToolchain') -ExpectedExitCode 1

$missing_root = Join-Path $test_root 'missing-archives'
New-Item -ItemType Directory -Force -Path $missing_root | Out-Null
Invoke-VpsValidator -Case required_archive_missing -Arguments @('-ManifestPath', $manifest_path, '-DownloadsRoot', $missing_root, '-RequireArchives', '-SkipToolchain') -ExpectedExitCode 1

[Console]::WriteLine('[vps] level=info event=test_suite_complete cases=4')
