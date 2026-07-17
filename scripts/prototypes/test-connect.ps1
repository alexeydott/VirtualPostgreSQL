[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64', 'all')]
    [string]$Architecture = 'all',
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$IncludeServerCases
)

. (Join-Path $PSScriptRoot '..\deps\vps-deps-common.ps1')

try {
    $repository_root = Get-VpsRepositoryRoot
    $architectures = if ($Architecture -eq 'all') { @('x86', 'x64') } else { @($Architecture) }
    $results = [System.Collections.Generic.List[object]]::new()
    foreach ($arch in $architectures) {
        $executable = Join-Path $repository_root "build\prototypes\$arch\$Configuration\vps_proto_connect.exe"
        if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
            throw "connect prototype is missing: arch=$arch config=$Configuration"
        }
        $port_probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
        $port_probe.Start()
        $previous_refused_port = $env:VPS_TEST_REFUSED_PORT
        $env:VPS_TEST_REFUSED_PORT = [string]([System.Net.IPEndPoint]$port_probe.LocalEndpoint).Port
        $port_probe.Stop()
        & $executable refused 5000
        $refused_exit = $LASTEXITCODE
        $env:VPS_TEST_REFUSED_PORT = $previous_refused_port
        if ($refused_exit -ne 0) {
            throw "offline connect case failed: arch=$arch case=refused exit=$refused_exit"
        }
        $results.Add([ordered]@{ architecture = $arch; configuration = $Configuration; case = 'refused'; outcome = 'pass' })

        foreach ($case in @('timeout', 'interrupt', 'partial')) {
            $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
            $listener.Start()
            $previous_stall_port = $env:VPS_TEST_STALL_PORT
            $env:VPS_TEST_STALL_PORT = [string]([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
            $deadline_ms = if ($case -eq 'timeout') { 250 } else { 1000 }
            $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
            & $executable $case $deadline_ms
            $exit_code = $LASTEXITCODE
            $stopwatch.Stop()
            $listener.Stop()
            $env:VPS_TEST_STALL_PORT = $previous_stall_port
            if ($exit_code -ne 0) {
                throw "offline connect case failed: arch=$arch case=$case exit=$exit_code"
            }
            if ($case -eq 'timeout' -and $stopwatch.ElapsedMilliseconds -gt 1250) {
                throw "connect deadline bound exceeded: arch=$arch elapsed_ms=$($stopwatch.ElapsedMilliseconds)"
            }
            $results.Add([ordered]@{ architecture = $arch; configuration = $Configuration; case = $case; outcome = 'pass'; elapsed_ms = $stopwatch.ElapsedMilliseconds })
        }
        if ($IncludeServerCases) {
            foreach ($case in @('success', 'auth-failure')) {
                & $executable $case 5000
                if ($LASTEXITCODE -ne 0) {
                    throw "server connect case failed: arch=$arch case=$case exit=$LASTEXITCODE"
                }
                $results.Add([ordered]@{ architecture = $arch; configuration = $Configuration; case = $case; outcome = 'pass' })
            }
        }
    }
    Write-VpsJsonFile -Path (Join-Path $repository_root 'build\prototype-evidence\connect.json') -Value ([ordered]@{
        schema_version = 1
        server_cases = $IncludeServerCases.IsPresent
        results = @($results)
    })
} catch {
    Write-VpsBuildLog -Level error -Event connect_tests_failed -Fields @{ reason = $_.Exception.Message }
    exit 1
}
