[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64', 'all')]
    [string]$Architecture = 'all',
    [ValidateSet('Release', 'Debug', 'all')]
    [string]$Configuration = 'all',
    [switch]$Clean
)

. (Join-Path $PSScriptRoot '..\deps\vps-deps-common.ps1')

try {
    $repository_root = Get-VpsRepositoryRoot
    $source_root = Join-Path $repository_root 'tools\prototypes'
    $architectures = if ($Architecture -eq 'all') { @('x86', 'x64') } else { @($Architecture) }
    $configurations = if ($Configuration -eq 'all') { @('Release', 'Debug') } else { @($Configuration) }
    foreach ($arch in $architectures) {
        foreach ($config in $configurations) {
            $build_root = Join-Path $repository_root "build\prototypes\$arch\$config"
            $deps_root = Join-Path $repository_root "build\deps\$arch\$config"
            $manifest_path = Join-Path $build_root 'manifest.json'
            if ($Clean) {
                $null = Reset-VpsBuildDirectory -Path $build_root
            } elseif (-not (Test-Path -LiteralPath $build_root -PathType Container)) {
                New-Item -ItemType Directory -Force -Path $build_root | Out-Null
            }
            $dev_cmd = Get-VpsVcVarsBatch -Architecture $arch
            $batch_path = Join-Path $build_root 'build.cmd'
            $batch_lines = @(
                '@echo off',
                "call `"$dev_cmd`" >nul",
                'if errorlevel 1 exit /b %errorlevel%',
                "cmake -S `"$source_root`" -B `"$build_root`" -G Ninja -DCMAKE_BUILD_TYPE=$config -DVPS_DEPS_ROOT=`"$deps_root`"",
                'if errorlevel 1 exit /b %errorlevel%',
                "cmake --build `"$build_root`" --config $config",
                'exit /b %errorlevel%'
            )
            Invoke-VpsBatchFile -Path $batch_path -Lines $batch_lines -Phase prototype_build -Architecture $arch -Configuration $config
            $executables = @(Get-ChildItem -LiteralPath $build_root -File -Filter 'vps_proto_*.exe')
            if ($executables.Count -eq 0) {
                throw "prototype executables are missing: arch=$arch config=$config"
            }
            $artifacts = foreach ($executable in $executables) {
                [ordered]@{
                    name = $executable.Name
                    sha256 = (Get-FileHash -LiteralPath $executable.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                    bytes = $executable.Length
                }
            }
            Write-VpsJsonFile -Path $manifest_path -Value ([ordered]@{
                schema_version = 1
                architecture = $arch
                configuration = $config
                runtime = if ($config -eq 'Release') { '/MT' } else { '/MTd' }
                artifacts = @($artifacts)
            })
            Write-VpsBuildLog -Level info -Event prototype_manifest_written -Fields @{ arch = $arch; config = $config; artifacts = $executables.Count }
        }
    }
} catch {
    Write-VpsBuildLog -Level error -Event prototype_build_failed -Fields @{ reason = $_.Exception.Message }
    exit 1
}
