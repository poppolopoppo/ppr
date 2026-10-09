[CmdletBinding()]
param(
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$machineName = 'podman-machine-default'
$connectionName = 'podman-machine-default'
$containerNames = @('searxng')

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

# Microsoft WinDbg and its user-owned sessions are independent of this stack.

# 1c. Best-effort pix-mcp binary cache removal (host-native stdio MCP; no containers, no daemon).
# Removes the downloaded exe/zip/staging only; the version.txt pin is committed
# config and captures/ is user data, so both are preserved. Never fails uninstall.
try {
    $pixDir = Join-Path $projectRoot 'local-services\pix'
    if (-not (Test-Path -LiteralPath $pixDir -PathType Container)) {
        Write-Log 'local-services\pix not found; skipping pix-mcp cleanup.'
    } else {
        $pixExe = Join-Path $pixDir 'pix-mcp.exe'
        $pixStaging = Join-Path $pixDir '_staging'
        if (Test-Path -LiteralPath $pixExe -PathType Leaf) {
            Write-Log 'Removing pix-mcp.exe binary cache (best-effort)...'
            Remove-Item -LiteralPath $pixExe -Force
        } else {
            Write-Log 'pix-mcp.exe not found; skipping binary removal.'
        }
        foreach ($leftover in @(Get-ChildItem -LiteralPath $pixDir -Filter 'pix-mcp-*.zip' -ErrorAction SilentlyContinue)) {
            Write-Log "  Removing stale archive $($leftover.Name)..."
            Remove-Item -LiteralPath $leftover.FullName -Force
        }
        if (Test-Path -LiteralPath $pixStaging) {
            Write-Log '  Removing pix staging directory...'
            Remove-Item -LiteralPath $pixStaging -Recurse -Force
        }
    }
} catch {
    Write-Log 'pix-mcp cache cleanup failed; continuing uninstall.'
}

# 2. Prune networks (optional, safe)
if (Get-Command podman -ErrorAction SilentlyContinue) {
    Write-Log 'Pruning unused networks...'
    & podman --connection $connectionName network prune -f | Out-Null
    Write-Log 'Networks pruned.'
}

# 3. Delete local-services/runtime-report.txt
$reportPath = Join-Path $projectRoot 'local-services\runtime-report.txt'
if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
    Remove-Item -LiteralPath $reportPath -Force
    Write-Log 'Deleted local-services/runtime-report.txt'
}

# 4. Optionally stop the default machine (only if no other containers running)
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