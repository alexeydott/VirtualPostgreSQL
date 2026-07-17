[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64')]
    [string]$Architecture,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateRange(0, 3)]
    [int]$MillionRuns = 0,
    [ValidateRange(1, 20)]
    [int]$CancelRepeat = 10,
    [switch]$Resume
)

. (Join-Path $PSScriptRoot '..\deps\vps-deps-common.ps1')

try {
    $repository_root = Get-VpsRepositoryRoot
    foreach ($name in @('VPS_TEST_HOST', 'VPS_TEST_PORT', 'VPS_TEST_USER', 'VPS_TEST_PASSWORD', 'VPS_TEST_DATABASE', 'VPS_TEST_SSLMODE')) {
        if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
            throw "server test environment is incomplete: field=$name"
        }
    }
    if ($env:VPS_TEST_SSLMODE -cne 'disable') {
        throw 'Stage 0 no-SSL stand requires VPS_TEST_SSLMODE=disable'
    }

    $binary_root = Join-Path $repository_root "build\prototypes\$Architecture\$Configuration"
    $evidence_path = Join-Path $repository_root "build\prototype-evidence\$Architecture-$($Configuration.ToLowerInvariant())"
    if ($Resume) {
        $evidence_root = Assert-VpsPathUnderBuild -Path $evidence_path
        New-Item -ItemType Directory -Force -Path $evidence_root | Out-Null
    } else {
        $evidence_root = Reset-VpsBuildDirectory -Path $evidence_path
    }
    $results = [System.Collections.Generic.List[object]]::new()

    function Invoke-VpsPrototypeCase {
        param(
            [string]$ExecutableName,
            [string]$Case,
            [string]$EvidenceName
        )
        $executable = Join-Path $binary_root $ExecutableName
        if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
            throw "prototype executable is missing: path=$executable"
        }
        $log_path = Join-Path $evidence_root "$EvidenceName.log"
        if ($Resume -and (Test-Path -LiteralPath $log_path -PathType Leaf)) {
            $existing_text = Get-Content -LiteralPath $log_path -Raw
            if ($existing_text -match '\[vps\]\s+level=error') {
                throw "cannot resume failed prototype evidence: evidence=$EvidenceName"
            }
            $results.Add([ordered]@{
                executable = $ExecutableName
                case = $Case
                duration_ms = $null
                log = Get-VpsRelativePath -BasePath $evidence_root -Path $log_path
                outcome = 'pass'
            })
            return
        }
        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        $output = if ([string]::IsNullOrEmpty($Case)) { & $executable 2>&1 } else { & $executable $Case 2>&1 }
        $exit_code = $LASTEXITCODE
        $stopwatch.Stop()
        $output | Set-Content -LiteralPath $log_path -Encoding utf8NoBOM
        $output | ForEach-Object { [Console]::WriteLine($_) }
        $log_text = $output | Out-String
        if (-not [string]::IsNullOrEmpty($env:VPS_TEST_PASSWORD)) {
            $secret_pattern = '(?<![A-Za-z0-9])' + [regex]::Escape($env:VPS_TEST_PASSWORD) + '(?![A-Za-z0-9])'
            if ($log_text -match $secret_pattern) {
                throw "secret token detected in prototype log: evidence=$EvidenceName"
            }
        }
        if ($log_text -match '(?i)(postgres(?:ql)?://|password\s*=|\bSELECT\s+|\bBEGIN\s*;|\bCOMMIT\s*;)') {
            throw "forbidden connection or SQL text detected in prototype log: evidence=$EvidenceName"
        }
        if ($exit_code -ne 0) {
            throw "prototype case failed: executable=$ExecutableName case=$Case exit=$exit_code"
        }
        $results.Add([ordered]@{
            executable = $ExecutableName
            case = $Case
            duration_ms = $stopwatch.ElapsedMilliseconds
            log = Get-VpsRelativePath -BasePath $evidence_root -Path $log_path
            outcome = 'pass'
        })
    }

    Invoke-VpsPrototypeCase 'vps_proto_query.exe' 'all' 'query-all'
    foreach ($case in @('before-send', 'wait', 'first-row', 'between-rows', 'cancel-timeout', 'broken-network')) {
        Invoke-VpsPrototypeCase 'vps_proto_cancel.exe' $case "cancel-$case"
    }
    for ($iteration = 2; $iteration -le $CancelRepeat; $iteration++) {
        Invoke-VpsPrototypeCase 'vps_proto_cancel.exe' 'wait' "cancel-wait-repeat-$iteration"
    }
    foreach ($case in @('normal', 'close-one', 'one-failure')) {
        Invoke-VpsPrototypeCase 'vps_proto_concurrency.exe' $case "concurrency-$case"
    }
    foreach ($case in @('commit', 'rollback', 'savepoint', 'outside-error', 'invalid-release', 'active-stream', 'loss-before-terminal', 'loss-during-terminal')) {
        Invoke-VpsPrototypeCase 'vps_proto_transaction.exe' $case "transaction-$case"
    }
    Invoke-VpsPrototypeCase 'vps_proto_postgis.exe' '' 'postgis-capability'
    foreach ($case in @('loss-before-row', 'loss-after-row', 'loss-in-transaction')) {
        Invoke-VpsPrototypeCase 'vps_proto_stream.exe' $case "stream-$case"
    }
    for ($run = 1; $run -le $MillionRuns; $run++) {
        Invoke-VpsPrototypeCase 'vps_proto_stream.exe' 'million' "stream-million-$run"
    }

    $dev_cmd = Get-VpsVcVarsBatch -Architecture $Architecture
    $inspection_root = Join-Path $evidence_root 'inspection'
    New-Item -ItemType Directory -Force -Path $inspection_root | Out-Null
    foreach ($executable in Get-ChildItem -LiteralPath $binary_root -File -Filter 'vps_proto_*.exe') {
        $dependents_path = Join-Path $inspection_root "$($executable.BaseName)-dependents.txt"
        $batch_path = Join-Path $inspection_root "$($executable.BaseName)-inspect.cmd"
        Invoke-VpsBatchFile -Path $batch_path -Lines @(
            '@echo off',
            "call `"$dev_cmd`" >nul",
            'if errorlevel 1 exit /b %errorlevel%',
            "dumpbin /dependents `"$($executable.FullName)`" > `"$dependents_path`"",
            'exit /b %errorlevel%'
        ) -Phase prototype_import_inspection -Architecture $Architecture -Configuration $Configuration
        $dependents = Get-Content -LiteralPath $dependents_path -Raw
        if ($dependents -match '(?i)(libpq|libcrypto|libssl|zlib)[^\r\n]*\.dll') {
            throw "unexpected dependency DLL: executable=$($executable.Name)"
        }
    }

    Write-VpsJsonFile -Path (Join-Path $evidence_root 'manifest.json') -Value ([ordered]@{
        schema_version = 1
        architecture = $Architecture
        configuration = $Configuration
        sslmode = 'disable'
        tls_gate_satisfied = $false
        cancel_repeat = $CancelRepeat
        million_runs = $MillionRuns
        results = @($results)
    })
    Write-VpsBuildLog -Level info -Event stage0_prototype_gate_complete -Fields @{
        arch = $Architecture; config = $Configuration; cases = $results.Count; million_runs = $MillionRuns
    }
} catch {
    Write-VpsBuildLog -Level error -Event stage0_prototype_gate_failed -Fields @{ reason = $_.Exception.Message }
    exit 1
}
