Set-StrictMode -Version Latest

function Write-VpsCiEvent {
    param(
        [Parameter(Mandatory)][string]$Gate,
        [Parameter(Mandatory)][ValidateSet('debug', 'info', 'warn', 'error')][string]$Level,
        [Parameter(Mandatory)][string]$Status,
        [string]$FailureClass = '',
        [string]$RelativePath = '',
        [string]$Detail = ''
    )

    $configured = if ($env:VPS_CI_LOG_LEVEL) { $env:VPS_CI_LOG_LEVEL.ToLowerInvariant() } else { 'info' }
    $rank = @{ debug = 0; info = 1; warn = 2; error = 3; off = 4 }
    if (-not $rank.ContainsKey($configured)) {
        $configured = 'info'
    }
    if ($rank[$Level] -lt $rank[$configured]) {
        return
    }

    $fields = [Collections.Generic.List[string]]::new()
    $fields.Add("gate=$Gate")
    $fields.Add("level=$Level")
    $fields.Add("status=$Status")
    if ($FailureClass) { $fields.Add("failure_class=$FailureClass") }
    if ($RelativePath) { $fields.Add("path=$($RelativePath.Replace('\', '/'))") }
    if ($Detail) { $fields.Add("detail=$Detail") }
    Write-Information "[vps-ci] $($fields -join ' ')" -InformationAction Continue
}

function Get-VpsRelativePath {
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Path)

    return [IO.Path]::GetRelativePath(
        [IO.Path]::GetFullPath($Root),
        [IO.Path]::GetFullPath($Path)).Replace('\', '/')
}

function Assert-VpsSafeRoot {
    param([Parameter(Mandatory)][string]$Root)

    $resolved = Resolve-Path -LiteralPath $Root -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $resolved.Path -PathType Container)) {
        throw '[ci-root] scan root is not a directory'
    }
    return $resolved.Path
}
