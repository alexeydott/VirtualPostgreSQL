[CmdletBinding()]
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [ValidateSet('All', 'PlatformHeaders', 'FlatSrc', 'ForbiddenFiles', 'Licenses')]
    [string[]]$Check = @('All')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'vps-ci-common.ps1')

$gate = 'source-tree'
$rootPath = Assert-VpsSafeRoot -Root $Root
$failures = [Collections.Generic.List[object]]::new()

function Add-Failure {
    param([string]$Class, [string]$Path)
    $relative = Get-VpsRelativePath -Root $rootPath -Path $Path
    $script:failures.Add([pscustomobject]@{ Class = $Class; Path = $relative })
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass $Class -RelativePath $relative
}

function Test-Selected([string]$Name) {
    return $Check -contains 'All' -or $Check -contains $Name
}

$gitDirectory = Join-Path $rootPath '.git'
if (Test-Path -LiteralPath $gitDirectory) {
    $listed = & git -C $rootPath ls-files --cached --others --exclude-standard
    if ($LASTEXITCODE -ne 0) {
        throw '[source-tree] unable to enumerate repository files'
    }
    $files = @($listed | Where-Object { $_ } |
        ForEach-Object { Join-Path $rootPath $_ } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
} else {
    $files = @(Get-ChildItem -LiteralPath $rootPath -File -Recurse | Where-Object {
        $relativeCandidate = Get-VpsRelativePath -Root $rootPath -Path $_.FullName
        $relativeCandidate -notmatch '^(?:build|dist|out|\.git)/'
    } | ForEach-Object FullName)
}

if (Test-Selected 'FlatSrc') {
    $src = Join-Path $rootPath 'src'
    if (Test-Path -LiteralPath $src) {
        Get-ChildItem -LiteralPath $src -Directory -Recurse | ForEach-Object {
            Add-Failure -Class 'src_not_flat' -Path $_.FullName
        }
    }
}

if (Test-Selected 'PlatformHeaders') {
    $forbiddenInclude = '(?im)^\s*#\s*include\s*[<"](?:windows\.h|winsock2?\.h|ws2tcpip\.h|bcrypt\.h|unistd\.h|pthread\.h|android/[^>"]+|libpq-fe\.h)[>"]'
    $platformBoundaryFiles = @(
        'src/vps_windows.c',
        'src/vps_windows_temp.c',
        'src/vps_wincred_provider.c',
        'src/vps_libpq_client.c',
        'src/vps_libpq_client_conninfo.c',
        'src/vps_libpq_client_tls.c'
    )
    foreach ($file in $files) {
        $relative = Get-VpsRelativePath -Root $rootPath -Path $file
        if ($relative -notmatch '^(include/.*\.(?:h|c)|src/.*\.(?:h|c))$') { continue }
        if ($relative -in $platformBoundaryFiles) { continue }
        if ((Get-Content -LiteralPath $file -Raw) -match $forbiddenInclude) {
            Add-Failure -Class 'platform_header_leak' -Path $file
        }
    }
}

if (Test-Selected 'ForbiddenFiles') {
    $allowedDotFiles = @('.gitignore', '.gitmodules')
    $generatedExtensions = @('.obj', '.o', '.a', '.lib', '.dll', '.exe', '.so', '.dylib', '.pdb', '.ilk', '.idb', '.exp', '.tmp', '.temp', '.bak', '.orig', '.rej')
    foreach ($file in $files) {
        $relative = Get-VpsRelativePath -Root $rootPath -Path $file
        $segments = $relative -split '/'
        $isDotPath = @($segments | Where-Object { $_.StartsWith('.') }).Count -gt 0
        if ($isDotPath -and $relative -notin $allowedDotFiles) {
            Add-Failure -Class 'private_context_in_release_set' -Path $file
            continue
        }
        if ($relative -in @('AGENTS.md', 'CLAUDE.md', 'skills-lock.json', 'VirtualPostgreSQL_Technical_Specification.md')) {
            Add-Failure -Class 'private_context_in_release_set' -Path $file
            continue
        }
        if ([IO.Path]::GetExtension($relative).ToLowerInvariant() -in $generatedExtensions -or
            [IO.Path]::GetFileName($relative) -in @('CMakeCache.txt', 'build.ninja', 'compile_commands.json')) {
            Add-Failure -Class 'generated_file_in_release_set' -Path $file
        }
    }
}

if (Test-Selected 'Licenses') {
    $manifestPath = Join-Path $rootPath 'deps\versions.json'
    foreach ($required in @((Join-Path $rootPath 'LICENSE'), (Join-Path $rootPath 'licenses\README.md'), $manifestPath)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            Add-Failure -Class 'missing_license_input' -Path $required
        }
    }
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        try {
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            if (-not $manifest.dependencies -or @($manifest.dependencies).Count -eq 0) {
                Add-Failure -Class 'empty_dependency_manifest' -Path $manifestPath
            } else {
                foreach ($dependency in $manifest.dependencies) {
                    if (-not $dependency.name -or -not $dependency.license -or -not $dependency.license_file) {
                        Add-Failure -Class 'missing_dependency_license_metadata' -Path $manifestPath
                    }
                }
            }
        } catch {
            Add-Failure -Class 'invalid_dependency_manifest' -Path $manifestPath
        }
    }
}

if ($failures.Count -gt 0) {
    Write-VpsCiEvent -Gate $gate -Level error -Status failed -FailureClass 'gate_failure' -Detail "count=$($failures.Count)"
    exit 1
}

Write-VpsCiEvent -Gate $gate -Level info -Status passed -Detail "files=$($files.Count)"
