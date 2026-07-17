[CmdletBinding()]
param(
    [ValidateSet('msvc-x86-debug', 'msvc-x64-release', 'clang-cl-x64-debug')]
    [string]$Preset = 'msvc-x64-release',
    [string]$VisualStudioRoot = 'D:\Visual Studio2022'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$isX86 = $Preset -eq 'msvc-x86-debug'
$isClang = $Preset -eq 'clang-cl-x64-debug'
$architecture = if ($isX86) { 'x86' } else { 'x64' }
$vcvarsName = if ($isX86) { 'vcvars32.bat' } else { 'vcvars64.bat' }
$vcvars = Join-Path $VisualStudioRoot "VC\Auxiliary\Build\$vcvarsName"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "[stage1-build] Visual Studio environment script missing for $architecture"
}

$compilerSetup = ''
if ($isClang) {
    $llvmBin = Join-Path $VisualStudioRoot 'VC\Tools\Llvm\x64\bin'
    if (-not (Test-Path -LiteralPath (Join-Path $llvmBin 'clang-cl.exe'))) {
        throw '[stage1-build] clang-cl compiler missing'
    }
    $compilerSetup = "set `"PATH=$llvmBin;%PATH%`" && "
}

$started = [Diagnostics.Stopwatch]::StartNew()
Write-Information "[stage1-build] phase=start preset=$Preset arch=$architecture status=running" -InformationAction Continue
$command = "call `"$vcvars`" >nul && ${compilerSetup}cmake --fresh --preset $Preset && cmake --build --preset $Preset && ctest --preset $Preset"
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "[stage1-build] preset failed: $Preset"
}
$started.Stop()
Write-Information "[stage1-build] phase=complete preset=$Preset arch=$architecture duration_ms=$($started.ElapsedMilliseconds) status=passed" -InformationAction Continue
