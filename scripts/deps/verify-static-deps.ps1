[CmdletBinding()]
param(
    [switch]$TestWrongArchitecture,
    [switch]$TestWrongRuntime
)

. (Join-Path $PSScriptRoot 'vps-deps-common.ps1')

try {
    $repository_root = Get-VpsRepositoryRoot
    $verify_root = Reset-VpsBuildDirectory -Path (Join-Path $repository_root 'build\verify-static-deps')
    $results = [System.Collections.Generic.List[object]]::new()

    foreach ($arch in @('x86', 'x64')) {
        foreach ($config in @('Release', 'Debug')) {
            $dev_cmd = Get-VpsVcVarsBatch -Architecture $arch
            $openssl_root = Join-Path $repository_root "build\deps\$arch\$config\openssl"
            $zlib_root = Join-Path $repository_root "build\deps\$arch\$config\zlib"
            $openssl_manifest_path = Join-Path $openssl_root 'manifest.json'
            $zlib_manifest_path = Join-Path $zlib_root 'manifest.json'
            if (-not (Test-Path -LiteralPath $openssl_manifest_path) -or -not (Test-Path -LiteralPath $zlib_manifest_path)) {
                throw "dependency manifest missing: arch=$arch config=$config"
            }

            $crypto_library = Get-ChildItem -LiteralPath $openssl_root -Recurse -File -Filter '*.lib' | Where-Object { $_.Name -match '^libcrypto.*\.lib$' } | Select-Object -First 1
            $ssl_library = Get-ChildItem -LiteralPath $openssl_root -Recurse -File -Filter '*.lib' | Where-Object { $_.Name -match '^libssl.*\.lib$' } | Select-Object -First 1
            $zlib_library = Get-Item -LiteralPath (Join-Path $zlib_root 'lib\zlib.lib')
            if ($null -eq $crypto_library -or $null -eq $ssl_library) {
                throw "OpenSSL libraries missing: arch=$arch config=$config"
            }

            $case_root = Join-Path $verify_root "$arch-$($config.ToLowerInvariant())"
            New-Item -ItemType Directory -Force -Path $case_root | Out-Null
            $probe_path = Join-Path $case_root 'static_probe.c'
            @'
#include <openssl/crypto.h>
#include <zlib.h>

int main(void)
{
    if (OpenSSL_version_num() == 0UL) {
        return 1;
    }
    return zlibVersion()[0] == '\0' ? 2 : 0;
}
'@ | Set-Content -LiteralPath $probe_path -Encoding ascii

            $runtime = if ($config -eq 'Release') { '/MT' } else { '/MTd' }
            $probe_exe = Join-Path $case_root 'static_probe.exe'
            $headers_path = Join-Path $case_root 'headers.txt'
            $directives_path = Join-Path $case_root 'zlib-directives.txt'
            $dependents_path = Join-Path $case_root 'dependents.txt'
            $batch_path = Join-Path $case_root 'verify.cmd'
            $batch_lines = @(
                '@echo off',
                "call `"$dev_cmd`" >nul",
                'if errorlevel 1 exit /b %errorlevel%',
                "cl /nologo /W4 /WX /sdl /TC $runtime `"$probe_path`" /I`"$(Join-Path $openssl_root 'include')`" /I`"$(Join-Path $zlib_root 'include')`" /link /OUT:`"$probe_exe`" `"$($crypto_library.FullName)`" `"$($ssl_library.FullName)`" `"$($zlib_library.FullName)`" ws2_32.lib gdi32.lib advapi32.lib crypt32.lib user32.lib",
                'if errorlevel 1 exit /b %errorlevel%',
                "`"$probe_exe`"",
                'if errorlevel 1 exit /b %errorlevel%',
                "dumpbin /headers `"$($crypto_library.FullName)`" > `"$headers_path`"",
                'if errorlevel 1 exit /b %errorlevel%',
                "dumpbin /directives `"$($zlib_library.FullName)`" > `"$directives_path`"",
                'if errorlevel 1 exit /b %errorlevel%',
                "dumpbin /dependents `"$probe_exe`" > `"$dependents_path`"",
                'exit /b %errorlevel%'
            )
            Invoke-VpsBatchFile -Path $batch_path -Lines $batch_lines -Phase static_dependency_probe -Architecture $arch -Configuration $config

            $headers = Get-Content -LiteralPath $headers_path -Raw
            $expected_machine = if ($arch -eq 'x86') { '14C machine \(x86\)' } else { '8664 machine \(x64\)' }
            if ($headers -notmatch $expected_machine) {
                throw "archive machine mismatch: arch=$arch config=$config"
            }
            $directives = Get-Content -LiteralPath $directives_path -Raw
            $expected_runtime_library = if ($config -eq 'Release') { 'LIBCMT' } else { 'LIBCMTD' }
            if ($directives -notmatch "(?i)/DEFAULTLIB:$expected_runtime_library(\s|$)") {
                throw "zlib runtime directive mismatch: arch=$arch config=$config expected=$expected_runtime_library"
            }
            if ($directives -match '(?i)/DEFAULTLIB:MSVCRTD?(\s|$)') {
                throw "dynamic runtime directive detected: arch=$arch config=$config"
            }
            $dependents = Get-Content -LiteralPath $dependents_path -Raw
            if ($dependents -match '(?i)(libcrypto|libssl|zlib)[^\r\n]*\.dll') {
                throw "unexpected dependency DLL: arch=$arch config=$config"
            }

            $results.Add([ordered]@{
                architecture = $arch
                configuration = $config
                runtime = $runtime
                executable_sha256 = (Get-FileHash -LiteralPath $probe_exe -Algorithm SHA256).Hash.ToLowerInvariant()
                external_crypto_dll = $false
                external_zlib_dll = $false
            })
            Write-VpsBuildLog -Level info -Event static_probe_validated -Fields @{ arch = $arch; config = $config; runtime = $runtime }
        }
    }

    if ($TestWrongArchitecture) {
        $dev_cmd = Get-VpsVcVarsBatch -Architecture x86
        $wrong_root = Join-Path $verify_root 'wrong-architecture'
        New-Item -ItemType Directory -Force -Path $wrong_root | Out-Null
        $source_path = Join-Path $wrong_root 'wrong_arch.c'
        'int main(void) { return 0; }' | Set-Content -LiteralPath $source_path -Encoding ascii
        $x64_crypto = Get-ChildItem -LiteralPath (Join-Path $repository_root 'build\deps\x64\Release\openssl') -Recurse -File -Filter '*.lib' | Where-Object { $_.Name -match '^libcrypto.*\.lib$' } | Select-Object -First 1
        $batch_path = Join-Path $wrong_root 'verify-wrong-arch.cmd'
        $batch_lines = @(
            '@echo off',
            "call `"$dev_cmd`" >nul",
            'if errorlevel 1 exit /b %errorlevel%',
            "cl /nologo /TC /MT `"$source_path`" `"$($x64_crypto.FullName)`" /link /OUT:`"$(Join-Path $wrong_root 'wrong_arch.exe')`" > `"$(Join-Path $wrong_root 'link.txt')`" 2>&1",
            'if not errorlevel 1 exit /b 91',
            'exit /b 0'
        )
        Invoke-VpsBatchFile -Path $batch_path -Lines $batch_lines -Phase wrong_architecture_rejection -Architecture x86 -Configuration Release
        Write-VpsBuildLog -Level info -Event wrong_architecture_rejected
    }

    if ($TestWrongRuntime) {
        $dev_cmd = Get-VpsVcVarsBatch -Architecture x64
        $wrong_root = Join-Path $verify_root 'wrong-runtime'
        New-Item -ItemType Directory -Force -Path $wrong_root | Out-Null
        $source_path = Join-Path $wrong_root 'wrong_runtime.c'
        @'
#include <zlib.h>
int main(void) { return zlibVersion()[0] == '\0'; }
'@ | Set-Content -LiteralPath $source_path -Encoding ascii
        $release_zlib_root = Join-Path $repository_root 'build\deps\x64\Release\zlib'
        $release_zlib = Join-Path $release_zlib_root 'lib\zlib.lib'
        $batch_path = Join-Path $wrong_root 'verify-wrong-runtime.cmd'
        $batch_lines = @(
            '@echo off',
            "call `"$dev_cmd`" >nul",
            'if errorlevel 1 exit /b %errorlevel%',
            "cl /nologo /TC /MTd `"$source_path`" /I`"$(Join-Path $release_zlib_root 'include')`" `"$release_zlib`" /link /WX /OUT:`"$(Join-Path $wrong_root 'wrong_runtime.exe')`" > `"$(Join-Path $wrong_root 'link.txt')`" 2>&1",
            'if not errorlevel 1 exit /b 92',
            'exit /b 0'
        )
        Invoke-VpsBatchFile -Path $batch_path -Lines $batch_lines -Phase wrong_runtime_rejection -Architecture x64 -Configuration Debug
        Write-VpsBuildLog -Level info -Event wrong_runtime_rejected
    }

    Write-VpsJsonFile -Path (Join-Path $verify_root 'manifest.json') -Value ([ordered]@{
        schema_version = 1
        results = @($results)
        wrong_architecture_tested = $TestWrongArchitecture.IsPresent
        wrong_runtime_tested = $TestWrongRuntime.IsPresent
    })
} catch {
    Write-VpsBuildLog -Level error -Event verification_failed -Fields @{ reason = $_.Exception.Message }
    exit 1
}
