[CmdletBinding()]
param(
    [switch] $SkipPrerequisiteInstall,
    [switch] $LaunchServices
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = $PSScriptRoot
$localServices = Join-Path $projectRoot 'local-services'
$wingetPackages = @(
    @{ Id = 'RedHat.Podman'; Name = 'Podman' },
    @{ Id = 'OpenJS.NodeJS.22'; Name = 'Node.js 22' },
    @{ Id = 'Python.Python.3.12'; Name = 'Python 3.12' }
)

function Stop-Setup([string] $message) {
    throw "Setup stopped: $message"
}

function Refresh-ProcessPath {
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = @($machinePath, $userPath) -join ';'
}

function Get-CommandPath([string] $name) {
    $command = Get-Command $name -ErrorAction SilentlyContinue
    if ($null -eq $command) { return $null }
    return $command.Source
}

function Get-Version([string] $command, [string[]] $arguments) {
    $output = & $command @arguments 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    $match = [regex]::Match(($output -join ' '), '(?<version>\d+\.\d+(?:\.\d+)?)')
    if (-not $match.Success) { return $null }
    return [version]$match.Groups['version'].Value
}

function Assert-MinimumVersion([string] $name, [string] $command, [string[]] $arguments, [version] $minimum) {
    $version = Get-Version $command $arguments
    if ($null -eq $version -or $version -lt $minimum) {
        Stop-Setup "$name $minimum or newer is required. Install it, open a new shell, and rerun install.ps1."
    }
    Write-Output "$name $version detected."
}

function Install-MissingWingetPackages([string[]] $missingIds) {
    if ($missingIds.Count -eq 0) { return }
    if ($SkipPrerequisiteInstall) {
        Stop-Setup "Missing prerequisites: $($missingIds -join ', '). Rerun without -SkipPrerequisiteInstall to install them."
    }
    foreach ($package in $wingetPackages | Where-Object { $_.Id -in $missingIds }) {
        Write-Output "Installing missing prerequisite $($package.Name) via WinGet $($package.Id)."
        & winget install --id $package.Id --exact --accept-source-agreements --accept-package-agreements
        if ($LASTEXITCODE -ne 0) { Stop-Setup "WinGet failed for $($package.Id)." }
    }
    Refresh-ProcessPath
}

function Assert-Layout {
    $required = @(
        'local-services\launch.ps1',
        'local-services\uninstall.ps1',
        'local-services\crawl4ai\config.yml',
        'local-services\searxng\settings.yml',
        '.opencode\package.json',
        '.opencode\package-lock.json'
    )
    $missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $projectRoot $_) -PathType Leaf) })
    if ($missing.Count -gt 0) { Stop-Setup "Required project files are missing: $($missing -join ', ')." }
}

function Assert-WindowsAndPrerequisites {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { Stop-Setup 'Windows is required.' }
    Assert-Layout

    $winget = Get-CommandPath 'winget'
    if ($null -eq $winget) { Stop-Setup 'WinGet is required. Install App Installer from Microsoft Store, then open a new shell.' }
    $wsl = Get-CommandPath 'wsl.exe'
    if ($null -eq $wsl) { Stop-Setup 'WSL is not installed. Manually run "wsl --install --no-distribution", reboot if requested, then rerun install.ps1.' }

    $missing = [System.Collections.Generic.List[string]]::new()
    Refresh-ProcessPath
    if ($null -eq (Get-CommandPath 'podman')) { $missing.Add('RedHat.Podman') }
    $node = Get-CommandPath 'node'
    $nodeVersion = if ($null -ne $node) { Get-Version $node @('--version') } else { $null }
    if ($null -eq $nodeVersion -or $nodeVersion.Major -lt 22) { $missing.Add('OpenJS.NodeJS.22') }
    $python = Get-CommandPath 'python'
    $pythonVersion = if ($null -ne $python) { Get-Version $python @('--version') } else { $null }
    if ($null -eq $pythonVersion -or $pythonVersion.Major -lt 3 -or ($pythonVersion.Major -eq 3 -and $pythonVersion.Minor -lt 12)) { $missing.Add('Python.Python.3.12') }
    Install-MissingWingetPackages $missing.ToArray()

    Refresh-ProcessPath
    $podman = Get-CommandPath 'podman'
    $node = Get-CommandPath 'node'
    $python = Get-CommandPath 'python'
    if ($null -eq $podman -or $null -eq $node -or $null -eq $python) {
        Stop-Setup 'A newly installed prerequisite is not on PATH. Open a new PowerShell shell and rerun install.ps1.'
    }
    Assert-MinimumVersion 'Node.js' $node @('--version') ([version]'22.0')
    Assert-MinimumVersion 'Python' $python @('--version') ([version]'3.12')
    Write-Output "Podman detected at $podman."
}

function Ensure-DefaultPodmanMachine {
    $machineName = 'podman-machine-default'
    $inspection = "$(& podman machine inspect $machineName --format '{{.State}} {{.Rootful}}' 2>$null)".Trim()
    $exists = $LASTEXITCODE -eq 0 -and $inspection.Length -gt 0
    if (-not $exists) {
        Write-Output "Default Podman machine '$machineName' not found. Initializing..."
        & podman machine init --rootful=false $machineName
        if ($LASTEXITCODE -ne 0) { Stop-Setup 'Default Podman Machine initialization failed.' }
    }
    $rootless = "$(& podman machine inspect $machineName --format '{{.Rootful}}' 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or $rootless -eq 'true') { Stop-Setup "Default machine $machineName is rootful; rootless is required." }
    $inspection = "$(& podman machine inspect $machineName --format '{{.State}}' 2>$null)".Trim()
    if ($inspection -ne 'running') {
        Write-Output "Starting default Podman machine '$machineName'..."
        & podman machine start $machineName
        if ($LASTEXITCODE -ne 0) { Stop-Setup 'Default Podman Machine start failed.' }
    }
    $rootless = "$(& podman --connection $machineName info --format '{{.Host.Security.Rootless}}' 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0 -or $rootless -ne 'true') { Stop-Setup 'Default Podman connection is not rootless.' }
    Write-Output "Default Podman Machine $machineName is running rootless."
}

Assert-WindowsAndPrerequisites
Ensure-DefaultPodmanMachine
Assert-MinimumVersion 'Node.js' (Get-CommandPath 'node') @('--version') ([version]'22.0')

if (-not $LaunchServices) {
    Write-Output 'Prerequisite and dependency gates passed; service launch is opt-in. Rerun with -LaunchServices to launch.'
    exit 0
}

$launch = Join-Path $localServices 'launch.ps1'
& pwsh -NoProfile -ExecutionPolicy Bypass -File $launch
if ($LASTEXITCODE -ne 0) { Stop-Setup 'local-services/launch.ps1 failed; inspect its redacted runtime-report.txt.' }
Write-Output 'Local services launched and runtime validation passed.'