[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DllPath,
    [Parameter(Mandatory)][ValidateSet('x86', 'x64')][string]$Architecture,
    [string]$VisualStudioRoot = 'D:\Visual Studio2022',
    [string]$ExpectedExportsPath = (Join-Path $PSScriptRoot 'expected-exports.txt'),
    [string]$AllowedImportsPath = (Join-Path $PSScriptRoot 'allowed-imports-release.txt'),
    [switch]$SkipImportCheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'windows-binary'
$dll = Resolve-Path -LiteralPath $DllPath -ErrorAction Stop
$vcvarsName = if ($Architecture -eq 'x86') { 'vcvars32.bat' } else { 'vcvars64.bat' }
$vcvars = Join-Path $VisualStudioRoot "VC\Auxiliary\Build\$vcvarsName"
foreach ($required in @($vcvars, $ExpectedExportsPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'missing_inspection_input' -RelativePath ([IO.Path]::GetFileName($required))
        exit 1
    }
}
if (-not $SkipImportCheck -and -not (Test-Path -LiteralPath $AllowedImportsPath -PathType Leaf)) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'missing_import_allowlist'
    exit 1
}

function Invoke-Dumpbin([string]$Mode) {
    $command = "call `"$vcvars`" >nul && dumpbin /nologo /$Mode `"$($dll.Path)`""
    $output = & cmd.exe /d /s /c $command 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "[windows-binary] dumpbin $Mode failed"
    }
    return @($output)
}

$headers = Invoke-Dumpbin 'headers'
$expectedMachine = if ($Architecture -eq 'x86') { '14C' } else { '8664' }
if (-not ($headers -match "(?i)^\s*$expectedMachine\s+machine\s+")) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'architecture_mismatch' -Detail "arch=$Architecture"
    exit 1
}

$exportLines = Invoke-Dumpbin 'exports'
$actualExports = @($exportLines | ForEach-Object {
    if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)(?:\s+=.*)?$') { $Matches[1] }
} | Where-Object { $_ } | Sort-Object -Unique)
$expectedExports = @(Get-Content -LiteralPath $ExpectedExportsPath | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
$exportDifference = @(Compare-Object -ReferenceObject $expectedExports -DifferenceObject $actualExports)
if ($exportDifference.Count -gt 0) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'unexpected_export_set' -Detail "expected=$($expectedExports.Count),actual=$($actualExports.Count)"
    exit 1
}

$credentialProviderAbiExports = @(
    'virtualpostgresql_credential_config_structure_size',
    'virtualpostgresql_credential_lease_structure_size',
    'virtualpostgresql_credential_provider_structure_size',
    'virtualpostgresql_register_credential_provider',
    'virtualpostgresql_wincred_provider'
)
$missingCredentialProviderAbiExports = @(
    $credentialProviderAbiExports | Where-Object { $_ -notin $actualExports }
)
$exportedWinCredInternals = @(
    $actualExports | Where-Object { $_ -like 'vps_wincred_*' }
)
if ($missingCredentialProviderAbiExports.Count -gt 0 -or
    $exportedWinCredInternals.Count -gt 0) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'credential_provider_export_boundary' -Detail "missing_abi=$($missingCredentialProviderAbiExports.Count),exported_wincred_internals=$($exportedWinCredInternals.Count)"
    exit 1
}

if (-not $SkipImportCheck) {
    $importLines = Invoke-Dumpbin 'imports'
    $actualImports = @($importLines | ForEach-Object {
        if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $Matches[1].ToUpperInvariant() }
    } | Sort-Object -Unique)
    $allowedImports = @(Get-Content -LiteralPath $AllowedImportsPath | ForEach-Object { $_.Trim().ToUpperInvariant() } | Where-Object { $_ } | Sort-Object -Unique)
    $unexpectedImports = @($actualImports | Where-Object { $_ -notin $allowedImports })
    if ($actualImports.Count -eq 0 -or $unexpectedImports.Count -gt 0) {
        Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'unexpected_import_set' -Detail "allowed=$($allowedImports.Count),actual=$($actualImports.Count)"
        exit 1
    }
}

$version = (Get-Item -LiteralPath $dll.Path).VersionInfo
if (-not $version.FileDescription -or -not $version.FileVersion -or
    -not $version.ProductName -or -not $version.ProductVersion -or
    $version.OriginalFilename -ne 'virtualpostgresql.dll') {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'incomplete_version_resource'
    exit 1
}

Write-VpsCiEvent -Gate $gate -Level info -Status passed -Detail "arch=$Architecture,exports=$($actualExports.Count),credential_provider_abi=exported,wincred_provider=exported,wincred_internals=hidden"
