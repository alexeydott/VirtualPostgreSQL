[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64', 'all')]
    [string]$Architecture = 'all',
    [ValidateSet('Release', 'Debug', 'all')]
    [string]$Configuration = 'all',
    [string]$ParserToolsRoot,
    [switch]$Clean
)

. (Join-Path $PSScriptRoot 'vps-deps-common.ps1')

try {
    $repository_root = Get-VpsRepositoryRoot
    & (Join-Path $PSScriptRoot 'versions.ps1') -RequireArchives
    if ($LASTEXITCODE -ne 0) {
        throw "dependency validation failed: exit=$LASTEXITCODE"
    }

    $manifest = Get-VpsDependencyManifest
    $pin = Get-VpsDependency -Name PostgreSQL
    $archive_path = Join-Path $repository_root "build\downloads\$($pin.archive)"
    $parser_root = if ([string]::IsNullOrWhiteSpace($ParserToolsRoot)) {
        if ([string]::IsNullOrWhiteSpace($env:VPS_PARSER_TOOLS_ROOT)) { [string]$manifest.toolchain.parser_tools_root } else { $env:VPS_PARSER_TOOLS_ROOT }
    } else {
        $ParserToolsRoot
    }
    $flex_path = Join-Path $parser_root 'flex.exe'
    $bison_path = Join-Path $parser_root 'bison.exe'
    foreach ($tool_path in @($flex_path, $bison_path)) {
        if (-not (Test-Path -LiteralPath $tool_path -PathType Leaf)) {
            throw "parser generator is missing: path=$tool_path"
        }
    }

    $architectures = if ($Architecture -eq 'all') { @('x86', 'x64') } else { @($Architecture) }
    $configurations = if ($Configuration -eq 'all') { @('Release', 'Debug') } else { @($Configuration) }
    foreach ($arch in $architectures) {
        foreach ($config in $configurations) {
            $dev_cmd = Get-VpsVcVarsBatch -Architecture $arch
            $install_root = Join-Path $repository_root "build\deps\$arch\$config\libpq"
            $work_root = Join-Path $repository_root "build\work\libpq-$arch-$($config.ToLowerInvariant())"
            $manifest_path = Join-Path $install_root 'manifest.json'
            if (-not $Clean -and (Test-Path -LiteralPath $manifest_path -PathType Leaf)) {
                Write-VpsBuildLog -Level info -Event build_skipped -Fields @{ component = 'libpq'; arch = $arch; config = $config }
                continue
            }

            $null = Reset-VpsBuildDirectory -Path $work_root
            $null = Reset-VpsBuildDirectory -Path $install_root
            tar -xf $archive_path -C $work_root
            if ($LASTEXITCODE -ne 0) {
                throw "PostgreSQL extraction failed: arch=$arch config=$config"
            }

            $source_root = Join-Path $work_root "postgresql-$($pin.version)"
            $build_root = Join-Path $work_root 'meson-build'
            $openssl_root = Join-Path $repository_root "build\deps\$arch\$config\openssl"
            $zlib_root = Join-Path $repository_root "build\deps\$arch\$config\zlib"
            foreach ($dependency_manifest in @((Join-Path $openssl_root 'manifest.json'), (Join-Path $zlib_root 'manifest.json'))) {
                if (-not (Test-Path -LiteralPath $dependency_manifest -PathType Leaf)) {
                    throw "static dependency is missing: path=$dependency_manifest"
                }
            }

            $runtime = if ($config -eq 'Release') { '/MT' } else { '/MTd' }
            $build_type = if ($config -eq 'Release') { 'release' } else { 'debug' }
            $vscrt = if ($config -eq 'Release') { 'mt' } else { 'mtd' }
            $meson_options = @(
                '--backend=ninja',
                "--buildtype=$build_type",
                '--default-library=static',
                '--auto-features=disabled',
                "-Db_vscrt=$vscrt",
                '-Dssl=openssl',
                '-Dnls=disabled',
                '-Dzlib=disabled',
                "-DFLEX=$($flex_path.Replace('\', '/'))",
                "-DBISON=$($bison_path.Replace('\', '/'))",
                "-Dextra_include_dirs=$((Join-Path $openssl_root 'include').Replace('\', '/'))",
                "-Dextra_lib_dirs=$((Join-Path $openssl_root 'lib').Replace('\', '/'))"
            )
            $quoted_options = $meson_options | ForEach-Object { "`"$_`"" }
            $batch_path = Join-Path $work_root 'build-libpq.cmd'
            $batch_lines = @(
                '@echo off',
                "call `"$dev_cmd`" >nul",
                'if errorlevel 1 exit /b %errorlevel%',
                'set PKG_CONFIG_PATH=',
                "set CMAKE_PREFIX_PATH=$openssl_root",
                'set LDFLAGS=ws2_32.lib crypt32.lib',
                "meson setup `"$build_root`" `"$source_root`" $($quoted_options -join ' ')",
                'if errorlevel 1 exit /b %errorlevel%',
                "meson compile -C `"$build_root`" libpq:static_library libpgcommon_shlib:static_library libpgport_shlib:static_library",
                'exit /b %errorlevel%'
            )
            Invoke-VpsBatchFile -Path $batch_path -Lines $batch_lines -Phase libpq_build -Architecture $arch -Configuration $config

            $targets_path = Join-Path $build_root 'meson-info\intro-targets.json'
            $targets = Get-Content -LiteralPath $targets_path -Raw | ConvertFrom-Json
            $selected_targets = [ordered]@{
                'libpq.lib' = @($targets | Where-Object { $_.name -ceq 'libpq' -and $_.type -ceq 'static library' })
                'pgcommon.lib' = @($targets | Where-Object { $_.name -ceq 'libpgcommon_shlib' -and $_.type -ceq 'static library' })
                'pgport.lib' = @($targets | Where-Object { $_.name -ceq 'libpgport_shlib' -and $_.type -ceq 'static library' })
            }
            $library_root = Join-Path $install_root 'lib'
            $include_root = Join-Path $install_root 'include'
            New-Item -ItemType Directory -Force -Path $library_root, $include_root | Out-Null
            $artifacts = [System.Collections.Generic.List[object]]::new()
            foreach ($output_name in $selected_targets.Keys) {
                $matches = @($selected_targets[$output_name])
                if ($matches.Count -ne 1 -or @($matches[0].filename).Count -ne 1) {
                    throw "Meson target is missing or ambiguous: target=$output_name count=$($matches.Count)"
                }
                $source_library = [string]$matches[0].filename[0]
                if (-not (Test-Path -LiteralPath $source_library -PathType Leaf)) {
                    throw "Meson target output is missing: target=$output_name"
                }
                $destination = Join-Path $library_root $output_name
                Copy-Item -LiteralPath $source_library -Destination $destination
                $file = Get-Item -LiteralPath $destination
                $artifacts.Add([ordered]@{
                    path = Get-VpsRelativePath -BasePath $install_root -Path $destination
                    sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
                    bytes = $file.Length
                })
            }

            foreach ($header in @(
                (Join-Path $source_root 'src\interfaces\libpq\libpq-fe.h'),
                (Join-Path $source_root 'src\interfaces\libpq\libpq-events.h'),
                (Join-Path $source_root 'src\include\postgres_ext.h')
            )) {
                Copy-Item -LiteralPath $header -Destination (Join-Path $include_root (Split-Path -Leaf $header))
            }
            Copy-Item -LiteralPath (Join-Path $source_root $pin.license_file) -Destination (Join-Path $install_root 'COPYRIGHT')
            if (Get-ChildItem -LiteralPath $install_root -Recurse -File -Filter 'libpq.dll') {
                throw "unexpected libpq DLL in static install contour: arch=$arch config=$config"
            }

            $openssl_manifest = Get-Content -LiteralPath (Join-Path $openssl_root 'manifest.json') -Raw | ConvertFrom-Json
            $zlib_manifest = Get-Content -LiteralPath (Join-Path $zlib_root 'manifest.json') -Raw | ConvertFrom-Json
            Write-VpsJsonFile -Path $manifest_path -Value ([ordered]@{
                schema_version = 1
                component = 'libpq'
                version = $pin.version
                source_archive = $pin.archive
                source_sha256 = $pin.sha256
                architecture = $arch
                configuration = $config
                runtime = $runtime
                linkage = 'static'
                meson_options = $meson_options
                parser_tools = [ordered]@{ flex = $manifest.toolchain.flex_version; bison = $manifest.toolchain.bison_version }
                features = [ordered]@{
                    ssl_openssl = $true
                    utf8 = $true
                    scram = $true
                    asynchronous_api = $true
                    single_row_mode = $true
                    secure_cancel = $true
                    thread_safe = $true
                    nls = $false
                    oauth_flow = $false
                }
                dependencies = @(
                    [ordered]@{ component = 'OpenSSL'; version = $openssl_manifest.version; source_sha256 = $openssl_manifest.source_sha256; role = 'direct' },
                    [ordered]@{ component = 'zlib'; version = $zlib_manifest.version; source_sha256 = $zlib_manifest.source_sha256; role = 'combined-probe-only' }
                )
                support_archives = @('pgcommon.lib', 'pgport.lib')
                artifacts = @($artifacts)
            })
            Write-VpsBuildLog -Level info -Event artifact_manifest_written -Fields @{
                component = 'libpq'; version = $pin.version; arch = $arch; config = $config; artifacts = $artifacts.Count; ssl = 'openssl'; nls = 'disabled'
            }
        }
    }
} catch {
    Write-VpsBuildLog -Level error -Event build_failed -Fields @{ component = 'libpq'; reason = $_.Exception.Message }
    exit 1
}
