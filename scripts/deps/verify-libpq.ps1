[CmdletBinding()]
param(
    [switch]$TestWrongArchitecture,
    [switch]$TestMissingFeature
)

. (Join-Path $PSScriptRoot 'vps-deps-common.ps1')

try {
    $repository_root = Get-VpsRepositoryRoot
    $probe_source = Join-Path $repository_root 'tools\prototypes\vps_libpq_link_probe.c'
    $verify_root = Reset-VpsBuildDirectory -Path (Join-Path $repository_root 'build\verify-libpq')
    $required_symbols = @(
        'PQconnectStartParams',
        'PQconnectPoll',
        'PQsetnonblocking',
        'PQsendPrepare',
        'PQsendDescribePrepared',
        'PQsendQueryPrepared',
        'PQsendQueryParams',
        'PQsetSingleRowMode',
        'PQgetResult',
        'PQcancelCreate',
        'PQcancelStart',
        'PQcancelPoll',
        'PQcancelStatus',
        'PQcancelSocket',
        'PQcancelReset',
        'PQcancelFinish',
        'PQisthreadsafe'
    )
    $results = [System.Collections.Generic.List[object]]::new()

    foreach ($arch in @('x86', 'x64')) {
        foreach ($config in @('Release', 'Debug')) {
            $dev_cmd = Get-VpsVcVarsBatch -Architecture $arch
            $libpq_root = Join-Path $repository_root "build\deps\$arch\$config\libpq"
            $openssl_root = Join-Path $repository_root "build\deps\$arch\$config\openssl"
            $zlib_root = Join-Path $repository_root "build\deps\$arch\$config\zlib"
            $manifest_path = Join-Path $libpq_root 'manifest.json'
            if (-not (Test-Path -LiteralPath $manifest_path -PathType Leaf)) {
                throw "libpq manifest is missing: arch=$arch config=$config"
            }
            $manifest = Get-Content -LiteralPath $manifest_path -Raw | ConvertFrom-Json
            if ($manifest.architecture -cne $arch -or $manifest.configuration -cne $config -or -not $manifest.features.thread_safe) {
                throw "libpq manifest contour mismatch: arch=$arch config=$config"
            }

            $libraries = [ordered]@{
                libpq = Join-Path $libpq_root 'lib\libpq.lib'
                pgcommon = Join-Path $libpq_root 'lib\pgcommon.lib'
                pgport = Join-Path $libpq_root 'lib\pgport.lib'
                ssl = Join-Path $openssl_root 'lib\libssl.lib'
                crypto = Join-Path $openssl_root 'lib\libcrypto.lib'
                zlib = Join-Path $zlib_root 'lib\zlib.lib'
            }
            foreach ($library in $libraries.Values) {
                if (-not (Test-Path -LiteralPath $library -PathType Leaf)) {
                    throw "static link input is missing: arch=$arch config=$config path=$library"
                }
            }
            if (Get-ChildItem -LiteralPath $libpq_root -Recurse -File -Filter 'libpq.dll') {
                throw "unexpected libpq DLL: arch=$arch config=$config"
            }

            $case_root = Join-Path $verify_root "$arch-$($config.ToLowerInvariant())"
            New-Item -ItemType Directory -Force -Path $case_root | Out-Null
            $probe_exe = Join-Path $case_root 'vps_libpq_link_probe.exe'
            $symbols_path = Join-Path $case_root 'symbols.txt'
            $headers_path = Join-Path $case_root 'headers.txt'
            $directives_path = Join-Path $case_root 'directives.txt'
            $dependents_path = Join-Path $case_root 'dependents.txt'
            $runtime = if ($config -eq 'Release') { '/MT' } else { '/MTd' }
            $batch_path = Join-Path $case_root 'verify.cmd'
            $batch_lines = @(
                '@echo off',
                "call `"$dev_cmd`" >nul",
                'if errorlevel 1 exit /b %errorlevel%',
                "cl /nologo /W4 /WX /sdl /TC $runtime `"$probe_source`" /I`"$(Join-Path $libpq_root 'include')`" /I`"$(Join-Path $openssl_root 'include')`" /I`"$(Join-Path $zlib_root 'include')`" /link /OUT:`"$probe_exe`" `"$($libraries.libpq)`" `"$($libraries.pgcommon)`" `"$($libraries.pgport)`" `"$($libraries.ssl)`" `"$($libraries.crypto)`" `"$($libraries.zlib)`" ws2_32.lib secur32.lib crypt32.lib advapi32.lib user32.lib shell32.lib",
                'if errorlevel 1 exit /b %errorlevel%',
                "`"$probe_exe`"",
                'if errorlevel 1 exit /b %errorlevel%',
                "dumpbin /linkermember:1 `"$($libraries.libpq)`" > `"$symbols_path`"",
                'if errorlevel 1 exit /b %errorlevel%',
                "dumpbin /headers `"$($libraries.libpq)`" > `"$headers_path`"",
                'if errorlevel 1 exit /b %errorlevel%',
                "dumpbin /directives `"$($libraries.libpq)`" > `"$directives_path`"",
                'if errorlevel 1 exit /b %errorlevel%',
                "dumpbin /dependents `"$probe_exe`" > `"$dependents_path`"",
                'exit /b %errorlevel%'
            )
            Invoke-VpsBatchFile -Path $batch_path -Lines $batch_lines -Phase libpq_static_probe -Architecture $arch -Configuration $config

            $symbols = Get-Content -LiteralPath $symbols_path -Raw
            foreach ($required_symbol in $required_symbols) {
                if ($symbols -notmatch [regex]::Escape($required_symbol)) {
                    throw "required libpq symbol is missing: arch=$arch config=$config symbol=$required_symbol"
                }
            }
            $headers = Get-Content -LiteralPath $headers_path -Raw
            $expected_machine = if ($arch -eq 'x86') { '14C machine \(x86\)' } else { '8664 machine \(x64\)' }
            $forbidden_machine = if ($arch -eq 'x86') { '8664 machine \(x64\)' } else { '14C machine \(x86\)' }
            if ($headers -notmatch $expected_machine -or $headers -match $forbidden_machine) {
                throw "libpq archive machine mismatch: arch=$arch config=$config"
            }
            $directives = Get-Content -LiteralPath $directives_path -Raw
            $expected_runtime_library = if ($config -eq 'Release') { 'LIBCMT' } else { 'LIBCMTD' }
            if ($directives -notmatch "(?i)/DEFAULTLIB:$expected_runtime_library(\s|$)") {
                throw "libpq runtime directive mismatch: arch=$arch config=$config expected=$expected_runtime_library"
            }
            if ($directives -match '(?i)/DEFAULTLIB:MSVCRTD?(\s|$)') {
                throw "dynamic CRT directive detected in libpq: arch=$arch config=$config"
            }
            $dependents = Get-Content -LiteralPath $dependents_path -Raw
            if ($dependents -match '(?i)(libpq|libcrypto|libssl|zlib)[^\r\n]*\.dll') {
                throw "unexpected dependency DLL in libpq probe: arch=$arch config=$config"
            }

            $results.Add([ordered]@{
                architecture = $arch
                configuration = $config
                runtime = $runtime
                required_symbols = $required_symbols.Count
                thread_safe = $true
                executable_sha256 = (Get-FileHash -LiteralPath $probe_exe -Algorithm SHA256).Hash.ToLowerInvariant()
                external_dependency_dll = $false
            })
            Write-VpsBuildLog -Level info -Event libpq_probe_validated -Fields @{
                version = $manifest.version; arch = $arch; config = $config; runtime = $runtime; symbols = $required_symbols.Count; thread_safe = $true
            }
        }
    }

    if ($TestWrongArchitecture) {
        $dev_cmd = Get-VpsVcVarsBatch -Architecture x86
        $case_root = Join-Path $verify_root 'wrong-architecture'
        New-Item -ItemType Directory -Force -Path $case_root | Out-Null
        $source_path = Join-Path $case_root 'wrong_arch.c'
        '#include <libpq-fe.h>' + [Environment]::NewLine + 'int main(void) { return PQlibVersion() == 0; }' |
            Set-Content -LiteralPath $source_path -Encoding ascii
        $x64_root = Join-Path $repository_root 'build\deps\x64\Release\libpq'
        $batch_path = Join-Path $case_root 'verify.cmd'
        $batch_lines = @(
            '@echo off',
            "call `"$dev_cmd`" >nul",
            'if errorlevel 1 exit /b %errorlevel%',
            "cl /nologo /TC /MT `"$source_path`" /I`"$(Join-Path $x64_root 'include')`" /link /OUT:`"$(Join-Path $case_root 'wrong_arch.exe')`" `"$(Join-Path $x64_root 'lib\libpq.lib')`" > `"$(Join-Path $case_root 'link.txt')`" 2>&1",
            'if not errorlevel 1 exit /b 91',
            'exit /b 0'
        )
        Invoke-VpsBatchFile -Path $batch_path -Lines $batch_lines -Phase libpq_wrong_architecture_rejection -Architecture x86 -Configuration Release
        Write-VpsBuildLog -Level info -Event wrong_architecture_rejected -Fields @{ component = 'libpq' }
    }

    if ($TestMissingFeature) {
        $dev_cmd = Get-VpsVcVarsBatch -Architecture x64
        $case_root = Join-Path $verify_root 'missing-feature'
        New-Item -ItemType Directory -Force -Path $case_root | Out-Null
        $source_path = Join-Path $case_root 'missing_feature.c'
        'extern int vps_required_secure_cancel_feature(void);' + [Environment]::NewLine + 'int main(void) { return vps_required_secure_cancel_feature(); }' |
            Set-Content -LiteralPath $source_path -Encoding ascii
        $batch_path = Join-Path $case_root 'verify.cmd'
        $batch_lines = @(
            '@echo off',
            "call `"$dev_cmd`" >nul",
            'if errorlevel 1 exit /b %errorlevel%',
            "cl /nologo /TC /MT `"$source_path`" /link /OUT:`"$(Join-Path $case_root 'missing_feature.exe')`" > `"$(Join-Path $case_root 'link.txt')`" 2>&1",
            'if not errorlevel 1 exit /b 92',
            'exit /b 0'
        )
        Invoke-VpsBatchFile -Path $batch_path -Lines $batch_lines -Phase missing_required_feature_rejection -Architecture x64 -Configuration Release
        Write-VpsBuildLog -Level info -Event missing_feature_rejected -Fields @{ component = 'libpq' }
    }

    Write-VpsJsonFile -Path (Join-Path $verify_root 'manifest.json') -Value ([ordered]@{
        schema_version = 1
        results = @($results)
        wrong_architecture_tested = $TestWrongArchitecture.IsPresent
        missing_feature_tested = $TestMissingFeature.IsPresent
    })
} catch {
    Write-VpsBuildLog -Level error -Event verification_failed -Fields @{ component = 'libpq'; reason = $_.Exception.Message }
    exit 1
}
