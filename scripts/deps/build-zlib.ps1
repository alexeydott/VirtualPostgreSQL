[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64', 'all')]
    [string]$Architecture = 'all',
    [ValidateSet('Release', 'Debug', 'all')]
    [string]$Configuration = 'all',
    [switch]$Clean
)

. (Join-Path $PSScriptRoot 'vps-deps-common.ps1')

try {
    $repository_root = Get-VpsRepositoryRoot
    & (Join-Path $PSScriptRoot 'versions.ps1') -RequireArchives
    if ($LASTEXITCODE -ne 0) {
        throw "dependency validation failed: exit=$LASTEXITCODE"
    }

    $pin = Get-VpsDependency -Name zlib
    $archive_path = Join-Path $repository_root "build\downloads\$($pin.archive)"
    $architectures = if ($Architecture -eq 'all') { @('x86', 'x64') } else { @($Architecture) }
    $configurations = if ($Configuration -eq 'all') { @('Release', 'Debug') } else { @($Configuration) }

    foreach ($arch in $architectures) {
        foreach ($config in $configurations) {
            $dev_cmd = Get-VpsVcVarsBatch -Architecture $arch
            $install_root = Join-Path $repository_root "build\deps\$arch\$config\zlib"
            $work_root = Join-Path $repository_root "build\work\zlib-$arch-$($config.ToLowerInvariant())"
            $manifest_path = Join-Path $install_root 'manifest.json'
            if (-not $Clean -and (Test-Path -LiteralPath $manifest_path -PathType Leaf)) {
                Write-VpsBuildLog -Level info -Event build_skipped -Fields @{ component = 'zlib'; arch = $arch; config = $config }
                continue
            }

            $null = Reset-VpsBuildDirectory -Path $work_root
            $null = Reset-VpsBuildDirectory -Path $install_root
            tar -xf $archive_path -C $work_root
            if ($LASTEXITCODE -ne 0) {
                throw "zlib extraction failed: arch=$arch config=$config"
            }

            $source_root = Join-Path $work_root "zlib-$($pin.version)"
            $runtime = if ($config -eq 'Release') { '/MT' } else { '/MTd' }
            $optimization = if ($config -eq 'Release') { '/O2 /DNDEBUG' } else { '/Od /D_DEBUG' }
            $batch_path = Join-Path $work_root 'build-zlib.cmd'
            $batch_lines = @(
                '@echo off',
                "call `"$dev_cmd`" >nul",
                'if errorlevel 1 exit /b %errorlevel%',
                "cd /d `"$source_root`"",
                'nmake -f win32\Makefile.msc clean >nul 2>&1',
                "nmake -f win32\Makefile.msc LOC=`"$runtime $optimization /D_CRT_SECURE_NO_WARNINGS`" zlib.lib",
                'exit /b %errorlevel%'
            )
            Invoke-VpsBatchFile -Path $batch_path -Lines $batch_lines -Phase zlib_build -Architecture $arch -Configuration $config

            $include_root = Join-Path $install_root 'include'
            $library_root = Join-Path $install_root 'lib'
            New-Item -ItemType Directory -Force -Path $include_root, $library_root | Out-Null
            Copy-Item -LiteralPath (Join-Path $source_root 'zlib.h'), (Join-Path $source_root 'zconf.h') -Destination $include_root
            Copy-Item -LiteralPath (Join-Path $source_root 'zlib.lib') -Destination (Join-Path $library_root 'zlib.lib')
            $library_path = Join-Path $library_root 'zlib.lib'

            $build_manifest = [ordered]@{
                schema_version = 1
                component = 'zlib'
                version = $pin.version
                source_archive = $pin.archive
                source_sha256 = $pin.sha256
                architecture = $arch
                configuration = $config
                runtime = $runtime
                linkage = 'static'
                compiler_flags = @($runtime, $optimization, '/D_CRT_SECURE_NO_WARNINGS')
                artifacts = @(
                    [ordered]@{
                        path = 'lib/zlib.lib'
                        sha256 = (Get-FileHash -LiteralPath $library_path -Algorithm SHA256).Hash.ToLowerInvariant()
                        bytes = (Get-Item -LiteralPath $library_path).Length
                    }
                )
            }
            Write-VpsJsonFile -Path $manifest_path -Value $build_manifest
            Write-VpsBuildLog -Level info -Event artifact_manifest_written -Fields @{
                component = 'zlib'
                arch = $arch
                config = $config
                artifacts = 1
            }
        }
    }
} catch {
    Write-VpsBuildLog -Level error -Event build_failed -Fields @{ component = 'zlib'; reason = $_.Exception.Message }
    exit 1
}
