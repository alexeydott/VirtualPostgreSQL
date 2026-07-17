Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:VpsBuildLogRanks = @{
    debug = 0
    info = 1
    warn = 2
    error = 3
    off = 4
}

function Get-VpsBuildLogLevel {
    $requested_level = $env:VPS_LOG_LEVEL
    if ([string]::IsNullOrWhiteSpace($requested_level)) {
        return 'info'
    }
    $normalized_level = $requested_level.ToLowerInvariant()
    if (-not $script:VpsBuildLogRanks.ContainsKey($normalized_level)) {
        return 'info'
    }
    return $normalized_level
}

function ConvertTo-VpsBuildLogValue {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return 'null'
    }
    return ([string]$Value).Replace("`r", ' ').Replace("`n", ' ').Replace(' ', '_')
}

function Write-VpsBuildLog {
    param(
        [ValidateSet('debug', 'info', 'warn', 'error')]
        [string]$Level,
        [string]$Event,
        [hashtable]$Fields = @{}
    )

    $configured_level = Get-VpsBuildLogLevel
    if ($configured_level -eq 'off' -or $script:VpsBuildLogRanks[$Level] -lt $script:VpsBuildLogRanks[$configured_level]) {
        return
    }

    $parts = [System.Collections.Generic.List[string]]::new()
    $parts.Add("level=$Level")
    $parts.Add("event=$Event")
    foreach ($key in ($Fields.Keys | Sort-Object)) {
        $parts.Add("$key=$(ConvertTo-VpsBuildLogValue -Value $Fields[$key])")
    }
    [Console]::WriteLine("[vps] $($parts -join ' ')")
}

function Get-VpsRepositoryRoot {
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
}

function Get-VpsDependencyManifest {
    $manifest_path = Join-Path (Get-VpsRepositoryRoot) 'deps\versions.json'
    return Get-Content -LiteralPath $manifest_path -Raw | ConvertFrom-Json
}

function Get-VpsDependency {
    param([string]$Name)

    $dependency = @(Get-VpsDependencyManifest).dependencies | Where-Object { $_.name -ceq $Name }
    if (@($dependency).Count -ne 1) {
        throw "dependency pin not found or ambiguous: name=$Name"
    }
    return $dependency
}

function Get-VpsVisualStudio2022 {
    $vswhere_path = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere_path -PathType Leaf)) {
        throw 'vswhere.exe is missing'
    }
    $json = & $vswhere_path -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -format json
    if ($LASTEXITCODE -ne 0) {
        throw "vswhere failed: exit=$LASTEXITCODE"
    }
    $instances = @($json | ConvertFrom-Json)
    if ($instances.Count -ne 1) {
        throw "expected one selected Visual Studio 2022 instance, found=$($instances.Count)"
    }
    return $instances[0]
}

function Get-VpsVcVarsBatch {
    param(
        [ValidateSet('x86', 'x64')]
        [string]$Architecture
    )

    $visual_studio = Get-VpsVisualStudio2022
    $file_name = if ($Architecture -eq 'x86') { 'vcvars32.bat' } else { 'vcvars64.bat' }
    $vcvars_path = Join-Path $visual_studio.installationPath "VC\Auxiliary\Build\$file_name"
    if (-not (Test-Path -LiteralPath $vcvars_path -PathType Leaf)) {
        throw "Visual C++ environment entry point is missing: architecture=$Architecture path=$vcvars_path"
    }
    return $vcvars_path
}

function Assert-VpsPathUnderBuild {
    param([string]$Path)

    $repository_root = Get-VpsRepositoryRoot
    $build_root = [System.IO.Path]::GetFullPath((Join-Path $repository_root 'build'))
    $resolved_path = [System.IO.Path]::GetFullPath($Path)
    $prefix = $build_root.TrimEnd('\') + '\'
    if (-not $resolved_path.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "path must stay under ignored build root: $resolved_path"
    }
    return $resolved_path
}

function Reset-VpsBuildDirectory {
    param([string]$Path)

    $resolved_path = Assert-VpsPathUnderBuild -Path $Path
    if (Test-Path -LiteralPath $resolved_path) {
        Remove-Item -LiteralPath $resolved_path -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $resolved_path | Out-Null
    return $resolved_path
}

function Invoke-VpsBatchFile {
    param(
        [string]$Path,
        [string[]]$Lines,
        [string]$Phase,
        [string]$Architecture,
        [string]$Configuration
    )

    $batch_path = Assert-VpsPathUnderBuild -Path $Path
    $batch_parent = Split-Path -Parent $batch_path
    New-Item -ItemType Directory -Force -Path $batch_parent | Out-Null
    $Lines | Set-Content -LiteralPath $batch_path -Encoding ascii

    $started_at = [System.Diagnostics.Stopwatch]::StartNew()
    Write-VpsBuildLog -Level info -Event phase_start -Fields @{
        phase = $Phase
        arch = $Architecture
        config = $Configuration
    }
    & cmd.exe /d /c $batch_path
    $exit_code = $LASTEXITCODE
    $started_at.Stop()
    if ($exit_code -ne 0) {
        Write-VpsBuildLog -Level error -Event phase_failed -Fields @{
            phase = $Phase
            arch = $Architecture
            config = $Configuration
            exit = $exit_code
            duration_ms = $started_at.ElapsedMilliseconds
        }
        throw "batch phase failed: phase=$Phase arch=$Architecture config=$Configuration exit=$exit_code"
    }
    Write-VpsBuildLog -Level info -Event phase_complete -Fields @{
        phase = $Phase
        arch = $Architecture
        config = $Configuration
        duration_ms = $started_at.ElapsedMilliseconds
    }
}

function Write-VpsJsonFile {
    param(
        [string]$Path,
        [object]$Value
    )

    $resolved_path = Assert-VpsPathUnderBuild -Path $Path
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolved_path) | Out-Null
    $Value | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $resolved_path -Encoding utf8NoBOM
}

function Get-VpsRelativePath {
    param(
        [string]$BasePath,
        [string]$Path
    )

    return [System.IO.Path]::GetRelativePath([System.IO.Path]::GetFullPath($BasePath), [System.IO.Path]::GetFullPath($Path)).Replace('\', '/')
}
