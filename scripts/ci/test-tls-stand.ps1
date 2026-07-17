[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/stage1-msvc-x64-release',
    [string]$OpenSslPath = 'openssl',
    [string]$TrustedCaFile = '',
    [switch]$Interactive
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$requiredEnvironment = @(
    'VPS_TLS_TEST_HOST',
    'VPS_TLS_TEST_PORT',
    'VPS_TLS_TEST_USER',
    'VPS_TLS_TEST_PASSWORD',
    'VPS_TLS_TEST_DBNAME'
)
$promptLabels = @{
    VPS_TLS_TEST_HOST = 'PostgreSQL host'
    VPS_TLS_TEST_PORT = 'PostgreSQL port'
    VPS_TLS_TEST_USER = 'PostgreSQL user'
    VPS_TLS_TEST_PASSWORD = 'PostgreSQL password'
    VPS_TLS_TEST_DBNAME = 'PostgreSQL database'
}
$promptedEnvironment = [Collections.Generic.List[string]]::new()
$temporary = $null
$key = $null
$certificate = $null
$openSslConfAdded = $false
$sslCertFileAdded = $false

try {
    foreach ($name in $requiredEnvironment) {
        $existing = [Environment]::GetEnvironmentVariable(
            $name, [EnvironmentVariableTarget]::Process)
        if (-not [string]::IsNullOrEmpty($existing)) {
            continue
        }
        if (-not $Interactive) {
            throw "[tls-stand] required runtime environment variable is missing: $name; rerun with -Interactive or set VPS_TLS_TEST_* in the parent process"
        }
        if ($name -eq 'VPS_TLS_TEST_PASSWORD') {
            $secureValue = Read-Host -Prompt $promptLabels[$name] -AsSecureString
            $pointer = [IntPtr]::Zero
            try {
                $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR(
                    $secureValue)
                $value = [Runtime.InteropServices.Marshal]::PtrToStringBSTR(
                    $pointer)
            } finally {
                if ($pointer -ne [IntPtr]::Zero) {
                    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
                }
                $secureValue.Dispose()
            }
        } else {
            $value = Read-Host -Prompt $promptLabels[$name]
        }
        if ([string]::IsNullOrEmpty($value)) {
            throw "[tls-stand] interactive value must not be empty: $name"
        }
        [Environment]::SetEnvironmentVariable(
            $name, $value, [EnvironmentVariableTarget]::Process)
        $value = $null
        $promptedEnvironment.Add($name)
    }

    $root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $buildCandidate = [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
    $rootPrefix = $root.TrimEnd([IO.Path]::DirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    if (-not $buildCandidate.StartsWith(
            $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw '[tls-stand] build directory must remain inside the repository root'
    }
    $build = (Resolve-Path -LiteralPath $buildCandidate -ErrorAction Stop).Path
    if (-not (Test-Path -LiteralPath $build -PathType Container)) {
        throw '[tls-stand] build directory is not a directory'
    }
    $probe = Join-Path $build 'vps_libpq_client_tls_integration_test.exe'
    $temporary = Join-Path $build 'tls-negative-fixture'
    $key = Join-Path $temporary 'unrelated-ca.key'
    $certificate = Join-Path $temporary 'unrelated-ca.pem'

    if (-not (Test-Path Env:OPENSSL_CONF)) {
        $openSslCommand = @(Get-Command $OpenSslPath `
                -CommandType Application -ErrorAction Stop)[0]
        $openSslRoot = Split-Path (Split-Path $openSslCommand.Source -Parent) `
            -Parent
        $openSslConfig = Join-Path $openSslRoot 'conf\openssl.cnf'
        if (Test-Path -LiteralPath $openSslConfig -PathType Leaf) {
            $env:OPENSSL_CONF = $openSslConfig
            $openSslConfAdded = $true
        }
    }
    if (-not (Test-Path Env:SSL_CERT_FILE)) {
        $caCandidate = $null
        if (-not [string]::IsNullOrWhiteSpace($TrustedCaFile)) {
            $caCandidate = [IO.Path]::GetFullPath($TrustedCaFile)
        } else {
            $gitCommand = @(Get-Command git -CommandType Application `
                    -ErrorAction SilentlyContinue)[0]
            if ($null -ne $gitCommand) {
                $gitRoot = Split-Path (Split-Path $gitCommand.Source -Parent) `
                    -Parent
                $caCandidate = Join-Path $gitRoot `
                    'mingw64\etc\ssl\certs\ca-bundle.crt'
            }
        }
        if ($null -eq $caCandidate -or
            -not (Test-Path -LiteralPath $caCandidate -PathType Leaf)) {
            throw '[tls-stand] trusted CA bundle not found; set SSL_CERT_FILE or pass -TrustedCaFile'
        }
        $env:SSL_CERT_FILE = $caCandidate
        $sslCertFileAdded = $true
    }

    New-Item -ItemType Directory -Path $temporary -Force | Out-Null
    & $OpenSslPath req -x509 -newkey rsa:2048 -nodes -days 1 `
        -subj '/CN=VirtualPostgreSQL Unrelated Test CA' `
        -keyout $key -out $certificate 2>$null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $certificate -PathType Leaf)) {
        throw '[tls-stand] unable to generate the unrelated public CA fixture'
    }
    Remove-Item -LiteralPath $key -Force
    $env:VPS_TLS_TEST_BAD_CA = $certificate
    if (-not (Test-Path $probe -PathType Leaf)) {
        throw "[tls-stand] integration probe is missing: $probe"
    }
    & $probe
    if ($LASTEXITCODE -ne 0) {
        throw '[tls-stand] certificate-enabled contour failed'
    }
} finally {
    Remove-Item Env:VPS_TLS_TEST_BAD_CA -ErrorAction SilentlyContinue
    foreach ($name in $promptedEnvironment) {
        Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue
    }
    if ($openSslConfAdded) {
        Remove-Item Env:OPENSSL_CONF -ErrorAction SilentlyContinue
    }
    if ($sslCertFileAdded) {
        Remove-Item Env:SSL_CERT_FILE -ErrorAction SilentlyContinue
    }
    if ($null -ne $key) {
        Remove-Item -LiteralPath $key -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $certificate) {
        Remove-Item -LiteralPath $certificate -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $temporary) {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

Write-Output '[tls-stand] status=passed probes=7 data_queries=0'
