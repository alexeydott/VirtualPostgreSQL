[CmdletBinding()]
param(
    [string]$ManifestPath,
    [string]$DownloadsRoot,
    [switch]$RequireArchives,
    [switch]$SkipToolchain
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:VpsLogRanks = @{
    debug = 0
    info = 1
    warn = 2
    error = 3
    off = 4
}

function Get-VpsLogLevel {
    $requested_level = $env:VPS_LOG_LEVEL
    if ([string]::IsNullOrWhiteSpace($requested_level)) {
        return 'info'
    }

    $normalized_level = $requested_level.ToLowerInvariant()
    if (-not $script:VpsLogRanks.ContainsKey($normalized_level)) {
        return 'info'
    }
    return $normalized_level
}

function ConvertTo-VpsLogValue {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return 'null'
    }
    return ([string]$Value).Replace("`r", ' ').Replace("`n", ' ').Replace(' ', '_')
}

function Write-VpsLog {
    param(
        [ValidateSet('debug', 'info', 'warn', 'error')]
        [string]$Level,
        [string]$Event,
        [hashtable]$Fields = @{}
    )

    $configured_level = Get-VpsLogLevel
    if ($configured_level -eq 'off' -or $script:VpsLogRanks[$Level] -lt $script:VpsLogRanks[$configured_level]) {
        return
    }

    $parts = [System.Collections.Generic.List[string]]::new()
    $parts.Add("level=$Level")
    $parts.Add("event=$Event")
    foreach ($key in ($Fields.Keys | Sort-Object)) {
        $parts.Add("$key=$(ConvertTo-VpsLogValue -Value $Fields[$key])")
    }
    [Console]::WriteLine("[vps] $($parts -join ' ')")
}

function Invoke-VpsVersionCommand {
    param(
        [string]$Name,
        [string[]]$Arguments = @()
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $command) {
        throw "required tool is missing: $Name"
    }

    Write-VpsLog -Level debug -Event tool_probe_start -Fields @{ tool = $Name }
    $output = & $command.Source @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "tool probe failed: $Name exit=$LASTEXITCODE"
    }
    Write-VpsLog -Level debug -Event tool_probe_complete -Fields @{ tool = $Name }
    return (($output | Out-String).Trim())
}

function Assert-VpsEqual {
    param(
        [string]$Name,
        [string]$Actual,
        [string]$Expected
    )

    if ($Actual -cne $Expected) {
        throw "$Name mismatch: expected=$Expected actual=$Actual"
    }
    Write-VpsLog -Level info -Event version_validated -Fields @{ component = $Name; version = $Actual }
}

function Get-VpsRegexVersion {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Name
    )

    $match = [regex]::Match($Text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $match.Success) {
        throw "could not parse $Name version"
    }
    return $match.Groups[1].Value
}

function Test-VpsToolchain {
    param([pscustomobject]$Expected)

    Assert-VpsEqual -Name powershell -Actual $PSVersionTable.PSVersion.ToString() -Expected $Expected.powershell_version

    $cmake_output = Invoke-VpsVersionCommand -Name cmake -Arguments @('--version')
    Assert-VpsEqual -Name cmake -Actual (Get-VpsRegexVersion -Text $cmake_output -Pattern 'cmake version\s+([0-9.]+)' -Name cmake) -Expected $Expected.cmake_version

    Assert-VpsEqual -Name ninja -Actual (Invoke-VpsVersionCommand -Name ninja -Arguments @('--version')).Trim() -Expected $Expected.ninja_version
    Assert-VpsEqual -Name meson -Actual (Invoke-VpsVersionCommand -Name meson -Arguments @('--version')).Trim() -Expected $Expected.meson_version

    $python_output = Invoke-VpsVersionCommand -Name py -Arguments @('-3', '--version')
    Assert-VpsEqual -Name python -Actual (Get-VpsRegexVersion -Text $python_output -Pattern 'Python\s+([0-9.]+)' -Name python) -Expected $Expected.python_version

    $perl_output = Invoke-VpsVersionCommand -Name perl -Arguments @('-e', 'print $^V')
    Assert-VpsEqual -Name perl -Actual $perl_output.TrimStart('v') -Expected $Expected.perl_version

    $nasm_output = Invoke-VpsVersionCommand -Name nasm -Arguments @('-v')
    Assert-VpsEqual -Name nasm -Actual (Get-VpsRegexVersion -Text $nasm_output -Pattern 'NASM version\s+([0-9.]+)' -Name nasm) -Expected $Expected.nasm_version

    $parser_tools_root = if ([string]::IsNullOrWhiteSpace($env:VPS_PARSER_TOOLS_ROOT)) {
        [string]$Expected.parser_tools_root
    } else {
        $env:VPS_PARSER_TOOLS_ROOT
    }
    $flex_path = Join-Path $parser_tools_root 'flex.exe'
    $bison_path = Join-Path $parser_tools_root 'bison.exe'
    foreach ($tool_path in @($flex_path, $bison_path)) {
        if (-not (Test-Path -LiteralPath $tool_path -PathType Leaf)) {
            throw "parser generator is missing: path=$tool_path"
        }
    }
    $flex_output = (& $flex_path --version 2>&1 | Out-String).Trim()
    $bison_output = (& $bison_path --version 2>&1 | Out-String).Trim()
    Assert-VpsEqual -Name flex -Actual (Get-VpsRegexVersion -Text $flex_output -Pattern '([0-9]+\.[0-9]+\.[0-9]+)\s*$' -Name flex) -Expected $Expected.flex_version
    Assert-VpsEqual -Name bison -Actual (Get-VpsRegexVersion -Text $bison_output -Pattern 'bison\D+([0-9]+\.[0-9]+\.[0-9]+)' -Name bison) -Expected $Expected.bison_version

    $tcl_root = if ([string]::IsNullOrWhiteSpace($env:VPS_TCL_ROOT)) { [string]$Expected.tcl_root } else { $env:VPS_TCL_ROOT }
    $tcl_path = Get-ChildItem -LiteralPath (Join-Path $tcl_root 'bin') -File -Filter 'tclsh*.exe' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($null -eq $tcl_path) {
        throw "Tcl shell is missing: root=$tcl_root"
    }
    $tcl_version = ('puts [info patchlevel]' | & $tcl_path.FullName | Out-String).Trim()
    Assert-VpsEqual -Name tcl -Actual $tcl_version -Expected $Expected.tcl_version

    $vswhere_path = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere_path -PathType Leaf)) {
        throw 'Visual Studio Installer vswhere.exe is missing'
    }

    $vs_json = & $vswhere_path -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -format json
    if ($LASTEXITCODE -ne 0) {
        throw "vswhere failed: exit=$LASTEXITCODE"
    }
    $instances = @($vs_json | ConvertFrom-Json)
    if ($instances.Count -ne 1) {
        throw "expected one selected Visual Studio 2022 instance, found=$($instances.Count)"
    }

    $instance = $instances[0]
    Assert-VpsEqual -Name visual_studio -Actual $instance.installationVersion -Expected $Expected.visual_studio_installation_version
    Assert-VpsEqual -Name visual_studio_product_line -Actual $instance.catalog.productLineVersion -Expected $Expected.visual_studio_product_line

    $vc_root = Join-Path $instance.installationPath "VC\Tools\MSVC\$($Expected.msvc_tools_version)"
    foreach ($architecture in @('x86', 'x64')) {
        $compiler_path = Join-Path $vc_root "bin\Hostx64\$architecture\cl.exe"
        if (-not (Test-Path -LiteralPath $compiler_path -PathType Leaf)) {
            throw "MSVC compiler missing: architecture=$architecture"
        }
        $compiler_version = (Get-Item -LiteralPath $compiler_path).VersionInfo.FileVersion
        Assert-VpsEqual -Name "msvc_$architecture" -Actual $compiler_version -Expected $Expected.msvc_compiler_version
    }

    $sdk_include = Join-Path 'C:\Windows Kits\10\Include' $Expected.windows_sdk_version
    if (-not (Test-Path -LiteralPath $sdk_include -PathType Container)) {
        throw "Windows SDK is missing: version=$($Expected.windows_sdk_version)"
    }
    Write-VpsLog -Level info -Event sdk_validated -Fields @{ version = $Expected.windows_sdk_version }
}

function Test-VpsArchives {
    param(
        [object[]]$Dependencies,
        [string]$Root,
        [bool]$ArchivesRequired
    )

    foreach ($dependency in $Dependencies) {
        if ($dependency.url -notmatch '^https://') {
            throw "dependency URL is not HTTPS: name=$($dependency.name)"
        }
        if ($dependency.sha256 -notmatch '^[0-9a-f]{64}$') {
            throw "dependency SHA-256 is invalid: name=$($dependency.name)"
        }

        $archive_path = Join-Path $Root $dependency.archive
        if (-not (Test-Path -LiteralPath $archive_path -PathType Leaf)) {
            if ($ArchivesRequired) {
                throw "required archive is missing: name=$($dependency.name) path=$archive_path"
            }
            Write-VpsLog -Level warn -Event archive_missing -Fields @{ name = $dependency.name; path = $archive_path }
            continue
        }

        $actual_hash = (Get-FileHash -LiteralPath $archive_path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual_hash -cne $dependency.sha256) {
            throw "archive hash mismatch: name=$($dependency.name) expected=$($dependency.sha256) actual=$actual_hash"
        }
        Write-VpsLog -Level info -Event archive_validated -Fields @{
            name = $dependency.name
            version = $dependency.version
            sha256 = $actual_hash
        }
    }
}

function Invoke-VpsValidation {
    $repository_root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
    $resolved_manifest = if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
        Join-Path $repository_root 'deps\versions.json'
    } else {
        [System.IO.Path]::GetFullPath($ManifestPath)
    }
    $resolved_downloads = if ([string]::IsNullOrWhiteSpace($DownloadsRoot)) {
        Join-Path $repository_root 'build\downloads'
    } else {
        [System.IO.Path]::GetFullPath($DownloadsRoot)
    }

    Write-VpsLog -Level info -Event validation_start -Fields @{ manifest = $resolved_manifest }
    if (-not (Test-Path -LiteralPath $resolved_manifest -PathType Leaf)) {
        throw "manifest is missing: $resolved_manifest"
    }

    $manifest = Get-Content -LiteralPath $resolved_manifest -Raw | ConvertFrom-Json
    if ($manifest.schema_version -ne 1) {
        throw "unsupported manifest schema: $($manifest.schema_version)"
    }
    $expected_dependencies = @('OpenSSL', 'PostgreSQL', 'SQLite', 'zlib')
    $actual_dependencies = @($manifest.dependencies | ForEach-Object { [string]$_.name } | Sort-Object -Unique)
    if (@(Compare-Object -ReferenceObject $expected_dependencies -DifferenceObject $actual_dependencies).Count -ne 0) {
        throw "pinned dependency set mismatch: expected=$($expected_dependencies.Count) actual=$($actual_dependencies.Count)"
    }
    foreach ($dependency in $manifest.dependencies) {
        if ([string]::IsNullOrWhiteSpace([string]$dependency.license) -or
            [string]::IsNullOrWhiteSpace([string]$dependency.license_file)) {
            throw "dependency license metadata is incomplete: name=$($dependency.name)"
        }
    }

    if (-not $SkipToolchain) {
        Test-VpsToolchain -Expected $manifest.toolchain
    } else {
        Write-VpsLog -Level warn -Event toolchain_validation_skipped
    }
    Test-VpsArchives -Dependencies @($manifest.dependencies) -Root $resolved_downloads -ArchivesRequired $RequireArchives.IsPresent
    Write-VpsLog -Level info -Event validation_complete -Fields @{ dependencies = @($manifest.dependencies).Count }
}

try {
    Invoke-VpsValidation
} catch {
    Write-VpsLog -Level error -Event validation_failed -Fields @{ reason = $_.Exception.Message }
    exit 1
}
