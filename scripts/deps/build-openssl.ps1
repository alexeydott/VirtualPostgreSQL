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

    $pin = Get-VpsDependency -Name OpenSSL
    $archive_path = Join-Path $repository_root "build\downloads\$($pin.archive)"
    $architectures = if ($Architecture -eq 'all') { @('x86', 'x64') } else { @($Architecture) }
    $configurations = if ($Configuration -eq 'all') { @('Release', 'Debug') } else { @($Configuration) }

    foreach ($arch in $architectures) {
        foreach ($config in $configurations) {
            $dev_cmd = Get-VpsVcVarsBatch -Architecture $arch
            $install_root = Join-Path $repository_root "build\deps\$arch\$config\openssl"
            $work_root = Join-Path $repository_root "build\work\openssl-$arch-$($config.ToLowerInvariant())"
            $manifest_path = Join-Path $install_root 'manifest.json'
            if (-not $Clean -and (Test-Path -LiteralPath $manifest_path -PathType Leaf)) {
                Write-VpsBuildLog -Level info -Event build_skipped -Fields @{ component = 'openssl'; arch = $arch; config = $config }
                continue
            }

            $null = Reset-VpsBuildDirectory -Path $work_root
            $null = Reset-VpsBuildDirectory -Path $install_root
            tar -xf $archive_path -C $work_root
            if ($LASTEXITCODE -ne 0) {
                throw "OpenSSL extraction failed: arch=$arch config=$config"
            }

            $source_root = Join-Path $work_root "openssl-$($pin.version)"
            $target = if ($arch -eq 'x86') { 'VC-WIN32' } else { 'VC-WIN64A' }
            $runtime = if ($config -eq 'Release') { '/MT' } else { '/MTd' }
            $mode = if ($config -eq 'Release') { '--release' } else { '--debug' }
            $prefix = $install_root.Replace('\', '/')
            $open_ssl_dir = (Join-Path $install_root 'ssl').Replace('\', '/')
            $batch_path = Join-Path $work_root 'build-openssl.cmd'
            $batch_lines = @(
                '@echo off',
                "call `"$dev_cmd`" >nul",
                'if errorlevel 1 exit /b %errorlevel%',
                "cd /d `"$source_root`"",
                "perl Configure $target $mode no-shared no-tests no-module no-docs no-makedepend --prefix=`"$prefix`" --openssldir=`"$open_ssl_dir`" $runtime",
                'if errorlevel 1 exit /b %errorlevel%',
                'nmake build_libs',
                'if errorlevel 1 exit /b %errorlevel%',
                'nmake install_dev',
                'exit /b %errorlevel%'
            )
            Invoke-VpsBatchFile -Path $batch_path -Lines $batch_lines -Phase openssl_build -Architecture $arch -Configuration $config

            $libraries = @(Get-ChildItem -LiteralPath $install_root -Recurse -File -Filter '*.lib' | Where-Object { $_.Name -match '^lib(crypto|ssl).*\.lib$' })
            if ($libraries.Count -lt 2) {
                throw "OpenSSL static libraries missing: arch=$arch config=$config count=$($libraries.Count)"
            }
            $artifacts = foreach ($library in $libraries) {
                [ordered]@{
                    path = Get-VpsRelativePath -BasePath $install_root -Path $library.FullName
                    sha256 = (Get-FileHash -LiteralPath $library.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                    bytes = $library.Length
                }
            }
            $build_manifest = [ordered]@{
                schema_version = 1
                component = 'OpenSSL'
                version = $pin.version
                source_archive = $pin.archive
                source_sha256 = $pin.sha256
                architecture = $arch
                configuration = $config
                runtime = $runtime
                target = $target
                linkage = 'static'
                configure = @($target, $mode, 'no-shared', 'no-tests', 'no-module', 'no-docs', 'no-makedepend', $runtime)
                artifacts = @($artifacts)
            }
            Write-VpsJsonFile -Path $manifest_path -Value $build_manifest
            Write-VpsBuildLog -Level info -Event artifact_manifest_written -Fields @{
                component = 'openssl'
                arch = $arch
                config = $config
                artifacts = $libraries.Count
            }
        }
    }
} catch {
    Write-VpsBuildLog -Level error -Event build_failed -Fields @{ component = 'openssl'; reason = $_.Exception.Message }
    exit 1
}
