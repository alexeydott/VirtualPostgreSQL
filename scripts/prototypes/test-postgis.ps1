[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64', 'all')]
    [string]$Architecture = 'all',
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

. (Join-Path $PSScriptRoot '..\deps\vps-deps-common.ps1')

try {
    foreach ($name in @('VPS_TEST_HOST', 'VPS_TEST_PORT', 'VPS_TEST_USER', 'VPS_TEST_PASSWORD',
                         'VPS_TEST_DATABASE', 'VPS_TEST_SSLMODE', 'VPS_TEST_POSTGIS_DATABASE')) {
        if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
            throw "PostGIS test environment is incomplete: field=$name"
        }
    }
    if ($env:VPS_TEST_SSLMODE -cne 'disable') {
        throw 'PostGIS test stand requires explicit sslmode=disable'
    }
    if ($env:VPS_TEST_DATABASE -ceq $env:VPS_TEST_POSTGIS_DATABASE) {
        throw 'absent and present PostGIS contours must use different databases'
    }

    $repository_root = Get-VpsRepositoryRoot
    $architectures = if ($Architecture -eq 'all') { @('x86', 'x64') } else { @($Architecture) }
    $evidence_root = Join-Path $repository_root 'build\prototype-evidence\postgis-matrix'
    New-Item -ItemType Directory -Path $evidence_root -Force | Out-Null
    $original_database = $env:VPS_TEST_DATABASE
    $results = [System.Collections.Generic.List[object]]::new()

    try {
        foreach ($arch in $architectures) {
            $executable = Join-Path $repository_root "build\prototypes\$arch\$Configuration\vps_proto_postgis.exe"
            if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
                throw "PostGIS prototype is missing: arch=$arch config=$Configuration"
            }
            foreach ($contour in @('absent', 'present')) {
                $env:VPS_TEST_DATABASE = if ($contour -eq 'absent') {
                    $original_database
                } else {
                    $env:VPS_TEST_POSTGIS_DATABASE
                }
                $log_path = Join-Path $evidence_root "postgis-$arch-$($Configuration.ToLowerInvariant())-$contour.log"
                & $executable 2>&1 | Set-Content -LiteralPath $log_path -Encoding utf8
                $exit_code = $LASTEXITCODE
                if ($exit_code -ne 0) {
                    throw "PostGIS prototype failed: arch=$arch contour=$contour exit=$exit_code"
                }
                $content = Get-Content -LiteralPath $log_path -Raw
                if ($content -match '(?i)postgres(?:ql)?://|\bpassword\s*=|\bSELECT\s+') {
                    throw "unsafe content in PostGIS evidence: arch=$arch contour=$contour"
                }
                if ($content -notmatch 'event=postgis_probe_complete outcome=pass' -or
                    $content -notmatch 'cleanup_exact=true') {
                    throw "incomplete PostGIS cleanup evidence: arch=$arch contour=$contour"
                }
                if ($contour -eq 'absent') {
                    if ($content -notmatch 'event=postgis_capability present=false') {
                        throw "absent PostGIS contour was not observed: arch=$arch"
                    }
                } else {
                    $geometry_passes = @([regex]::Matches(
                        $content,
                        'event=postgis_roundtrip kind=geometry .* outcome=pass'
                    )).Count
                    if ($content -notmatch 'event=postgis_capability present=true' -or
                        $geometry_passes -ne 14 -or
                        $content -notmatch 'kind=geography formats=wkt_wkb .* outcome=pass' -or
                        $content -notmatch 'event=postgis_null_empty .* outcome=pass' -or
                        $content -notmatch 'event=postgis_srid_mismatch .* decision=reject .* outcome=pass' -or
                        @([regex]::Matches($content, 'event=postgis_malformed .* outcome=pass')).Count -ne 2) {
                        throw "present PostGIS matrix is incomplete: arch=$arch geometry_passes=$geometry_passes"
                    }
                }
                $results.Add([ordered]@{
                    architecture = $arch
                    configuration = $Configuration
                    contour = $contour
                    outcome = 'pass'
                    log = Split-Path -Leaf $log_path
                })
            }
        }
    } finally {
        $env:VPS_TEST_DATABASE = $original_database
    }

    Write-VpsJsonFile -Path (Join-Path $evidence_root 'manifest.json') -Value ([ordered]@{
        schema_version = 1
        sslmode = 'disable'
        tls_gate_satisfied = $false
        results = @($results)
    })
    Write-VpsBuildLog -Level info -Event postgis_matrix_complete -Fields @{
        architectures = $architectures.Count
        cases = $results.Count
        configuration = $Configuration
    }
} catch {
    Write-VpsBuildLog -Level error -Event postgis_matrix_failed -Fields @{ reason = $_.Exception.Message }
    exit 1
}
