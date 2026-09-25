[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$reportPath = Join-Path $PSScriptRoot 'runtime-report.txt'
$machineName = 'podman-machine-default'
$connectionName = 'podman-machine-default'
$searxngSettings = Join-Path $PSScriptRoot 'searxng\settings.yml'
# Floating tags are intentional per user policy for searxng: images track :latest, no digest pins.
$searxngImage = 'ghcr.io/searxng/searxng:latest'
$secretNames = @('SEARXNG_SECRET')

$results = [System.Collections.Generic.List[string]]::new()

function Record([string] $name, [string] $status) {
    $results.Add("$name=$status")
}

function Stop-WithMessage([string] $message) {
    Record 'overall' 'blocked'
    $results | Set-Content -LiteralPath $reportPath -Encoding utf8
    throw $message
}

function Invoke-Safe([string] $name, [scriptblock] $operation) {
    try {
        & $operation
        Record $name 'passed'
    } catch {
        Record $name 'failed'
        throw
    }
}

function New-Secret {
    $bytes = [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
    return [Convert]::ToHexString($bytes).ToLowerInvariant()
}

function Invoke-Podman([string[]] $arguments) {
    & podman --connection $connectionName @arguments
}

function Ensure-MachineAndConnection {
    $inspection = "$(& podman machine inspect $machineName --format '{{.State}} {{.Rootful}}' 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($inspection)) {
        Write-Output "Initializing default Podman machine '$machineName'."
        & podman machine init --rootful=false $machineName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Default Podman machine initialization failed.' }
    }

    $rootful = "$(& podman machine inspect $machineName --format '{{.Rootful}}' 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or $rootful -eq 'true') {
        throw "Default Podman machine $machineName must be rootless."
    }

    $state = "$(& podman machine inspect $machineName --format '{{.State}}' 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Default Podman machine inspection failed.' }
    if ($state -ne 'running') {
        Write-Output "Starting default Podman machine '$machineName'."
        & podman machine start $machineName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Default Podman machine start failed.' }
    }

    $connectionJson = (& podman system connection list --format json 2>$null) -join "`n"
    $connections = @()
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($connectionJson)) {
        try { $connections = @($connectionJson | ConvertFrom-Json) } catch { $connections = @() }
    }
    $connection = @($connections | Where-Object { $_.Name -eq $connectionName })
    if ($connection.Count -eq 0) {
        # machine init/start normally creates this entry. Restarting the machine is
        # safe and lets Podman regenerate its rootless connection after metadata loss.
        & podman machine stop $machineName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Podman machine stop failed while restoring its connection.' }
        & podman machine start $machineName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Podman machine start failed while restoring its connection.' }
    }
}

function Assert-Connection {
    $connectionJson = (& podman system connection list --format json 2>$null) -join "`n"
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($connectionJson)) {
        throw 'The Podman connection listing failed.'
    }
    try { $connections = @($connectionJson | ConvertFrom-Json) } catch { throw 'The Podman connection listing was not valid JSON.' }
    $matches = @($connections | Where-Object { $_.Name -eq $connectionName })
    if ($matches.Count -ne 1) {
        throw "The $connectionName Podman connection could not be established."
    }
    $uri = [string]$matches[0].URI
    if ($uri -notmatch '/run/user/\d+/podman/podman\.sock$') {
        throw 'The Podman connection is not the expected rootless user socket.'
    }
    $rootless = "$(Invoke-Podman @('info', '--format', '{{.Host.Security.Rootless}}') 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or $rootless -ne 'true') { throw 'The Podman connection is not rootless.' }
    Record 'podman_connection' 'default_rootless_machine_verified'
}

function Assert-Machine {
    $machine = "$(& podman machine inspect $machineName --format '{{.State}} {{.Rootful}}' 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or $machine -notmatch '^running\s+False$') {
        throw "Default Podman machine must be running and rootless. Current: $machine"
    }
    Record 'podman_machine' 'running_rootless'
}

function Generate-Secrets {
    $secrets = @{}
    foreach ($name in $secretNames) {
        $secrets[$name] = New-Secret
    }
    if (@($secretNames | ForEach-Object { $secrets[$_] } | Select-Object -Unique).Count -ne 1) {
        throw 'Generated secrets must be distinct.'
    }
    return $secrets
}

function Start-SearXNG([hashtable] $secrets) {
    $containerName = 'searxng'
    $exists = "$(Invoke-Podman @('ps', '-a', '--filter', "name=^$containerName$", '--format', '{{.Names}}') 2>$null)".Trim()
    if ($exists) {
        Invoke-Podman @('rm', '-f', $containerName) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed to remove the existing SearXNG container." }
    }
    Invoke-Podman @(
        'run', '-d',
        '--name', $containerName,
        '--restart', 'unless-stopped',
        '-p', '127.0.0.1:8080:8080',
        '-e', "SEARXNG_SECRET=$($secrets['SEARXNG_SECRET'])",
        '-e', 'SEARXNG_LIMITER=false',
        '-e', 'SEARXNG_BASE_URL=http://127.0.0.1:8080/',
        '-v', "${searxngSettings}:/etc/searxng/settings.yml:ro",
        $searxngImage
    )
    if ($LASTEXITCODE -ne 0) { throw "Failed to start SearXNG container." }
    Record 'searxng_started' 'passed'
}

# Host-native windbg-tool (Devolutions): win-x64/arm64 only, stdio-only MCP, cannot
# containerize (needs host kernel/TTD driver/elevation/DbgEng/host PIDs/named
# pipes). No ports or secrets; never log dump/trace contents, only statuses.
# Elevation note: `trace record` requires elevation via inline sudo
# (`sudo windbg-tool trace record ...`); New-Window sudo mode is rejected.
function Refresh-DotnetToolPath {
    $toolDir = Join-Path $HOME '.dotnet\tools'
    if ((Test-Path -LiteralPath $toolDir -PathType Container) -and ($env:Path -notlike "*$toolDir*")) {
        $env:Path = "$toolDir;$env:Path"
    }
}

function Ensure-WindbgTool {
    if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
        throw '.NET SDK (dotnet) is required for windbg-tool. Rerun install.ps1, open a new shell, and retry.'
    }
    Refresh-DotnetToolPath
    if (-not (Get-Command windbg-tool -ErrorAction SilentlyContinue)) {
        Write-Output 'Installing windbg-tool global tool...'
        & dotnet tool install -g Devolutions.WinDbg.Tool
        if ($LASTEXITCODE -ne 0) { throw 'dotnet tool install -g Devolutions.WinDbg.Tool failed.' }
        Refresh-DotnetToolPath
    } else {
        & dotnet tool update -g Devolutions.WinDbg.Tool 2>$null
        Refresh-DotnetToolPath
    }
    if (-not (Get-Command windbg-tool -ErrorAction SilentlyContinue)) {
        throw 'windbg-tool is not on PATH after install/update. Open a new shell and rerun launch.ps1.'
    }
    & windbg-tool discover | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'windbg-tool discover failed.' }
    & windbg-tool daemon ensure | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'windbg-tool daemon ensure failed.' }
    Record 'windbg_tool_ensure' 'passed'
}

function Verify-WindbgTool {
    $deadline = (Get-Date).AddMinutes(2)
    $healthy = $false
    do {
        & windbg-tool --compact --envelope discover 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) {
            $healthy = $true
        } elseif ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 3
        }
    } while (-not $healthy -and (Get-Date) -lt $deadline)
    if (-not $healthy) { throw 'windbg-tool discover health check failed.' }
    Record 'windbg_tool_discover' 'passed'
}

function Verify-SearXNG {
    $deadline = (Get-Date).AddMinutes(2)
    $search = $null
    do {
        try {
            $search = Invoke-WebRequest -Uri 'http://127.0.0.1:8080/search?q=smoke&format=json' -Headers @{ 'X-Real-IP' = '127.0.0.1' } -TimeoutSec 20
        } catch {
            $search = $null
        }
        if ($null -eq $search -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 3 }
    } while ($null -eq $search -and (Get-Date) -lt $deadline)
    if ($null -eq $search -or $search.StatusCode -ne 200 -or ($search.Content | ConvertFrom-Json).query -ne 'smoke') {
        throw 'SearXNG JSON check failed.'
    }
    Record 'searxng_json' 'passed'
}

function Assert-HostPorts {
    $mappings = Invoke-Podman @('port', 'searxng')
    if ($mappings -notmatch '127\.0\.0\.1:8080') {
        throw 'Port mapping policy check failed.'
    }
    Record 'port_mappings' 'loopback_only'
}

New-Item -ItemType Directory -Path $PSScriptRoot -Force | Out-Null
'' | Set-Content -LiteralPath $reportPath -Encoding utf8

if (-not (Get-Command podman -ErrorAction SilentlyContinue)) { Stop-WithMessage 'Podman is required.' }
Record 'podman' 'available'

Ensure-MachineAndConnection
Record 'podman_machine' 'ensured_running_rootless'
Assert-Connection
Assert-Machine

$secrets = Generate-Secrets

Start-SearXNG $secrets
Ensure-WindbgTool

Start-Sleep -Seconds 5

try {
    Verify-SearXNG
    Verify-WindbgTool
    Assert-HostPorts
} catch {
    $results | Set-Content -LiteralPath $reportPath -Encoding utf8
    throw
}

$results | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Output "Stack launched and runtime checks passed. Redacted report: $reportPath"
