[CmdletBinding()]
param(
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$machineName = 'podman-machine-default'
$connectionName = 'podman-machine-default'
$containerNames = @('searxng', 'crawl4ai')

function Write-Log([string] $message) {
    Write-Host "[$(Get-Date -Format 'HH:mm:ss')] $message"
}

function Confirm-Action([string] $message) {
    if ($Force) { return $true }
    $response = Read-Host "$message (y/N)"
    return $response -eq 'y' -or $response -eq 'Y'
}

Write-Log 'Starting uninstall of local services...'

# 1. Stop and remove containers
if (Get-Command podman -ErrorAction SilentlyContinue) {
    Write-Log "Stopping and removing containers: $($containerNames -join ', ')"
    foreach ($name in $containerNames) {
        $running = "$(& podman --connection $connectionName ps --filter "name=^$name$" --format '{{.Names}}' 2>$null)".Trim()
        if ($running) {
            Write-Log "  Stopping $name..."
            & podman --connection $connectionName stop $name | Out-Null
        }
        $exists = "$(& podman --connection $connectionName ps -a --filter "name=^$name$" --format '{{.Names}}' 2>$null)".Trim()
        if ($exists) {
            Write-Log "  Removing $name..."
            & podman --connection $connectionName rm -f $name | Out-Null
        }
    }
    Write-Log 'Containers cleaned up.'
} else {
    Write-Log 'Podman not found; skipping container cleanup.'
}

# 2. Prune networks (optional, safe)
if (Get-Command podman -ErrorAction SilentlyContinue) {
    Write-Log 'Pruning unused networks...'
    & podman --connection $connectionName network prune -f | Out-Null
    Write-Log 'Networks pruned.'
}

# 3. Clear User environment variable
Write-Log 'Clearing CRAWL4AI_API_TOKEN from User environment...'
[Environment]::SetEnvironmentVariable('CRAWL4AI_API_TOKEN', $null, 'User')
$env:CRAWL4AI_API_TOKEN = $null
Write-Log 'Environment variable cleared.'

# 4. Delete local-services/.env and runtime-report.txt
$envPath = Join-Path $projectRoot 'local-services\.env'
$reportPath = Join-Path $projectRoot 'local-services\runtime-report.txt'
if (Test-Path -LiteralPath $envPath -PathType Leaf) {
    if (Confirm-Action "Delete $envPath?") {
        Remove-Item -LiteralPath $envPath -Force
        Write-Log 'Deleted local-services/.env'
    }
}
if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
    Remove-Item -LiteralPath $reportPath -Force
    Write-Log 'Deleted local-services/runtime-report.txt'
}

# 5. Optionally stop the default machine (only if no other containers running)
if (Get-Command podman -ErrorAction SilentlyContinue) {
    $otherContainers = "$(& podman --connection $connectionName ps -a --format '{{.Names}}' 2>$null)".Trim()
    if (-not $otherContainers) {
        if (Confirm-Action "No other containers on $machineName. Stop the machine?") {
            Write-Log "Stopping $machineName..."
            & podman machine stop $machineName | Out-Null
            Write-Log 'Machine stopped.'
        }
    } else {
        Write-Log "Other containers exist on $machineName; leaving machine running."
    }
}

Write-Log 'Uninstall complete.'
Write-Log 'Note: The podman-net-usermode helper distro and podman-machine-default are preserved.'
Write-Log 'To fully reset Podman, run: podman machine reset --force (removes ALL machines and data).'