#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Profile compilation times for the PPR engine.

.DESCRIPTION
    Builds a target with timing instrumentation and collects per-file
    compilation durations. Two collection modes:

      btplus (default) — sets CL=/Bt+, parses front/back/total from
                         the build log (no admin required).

      timetrace        — runs vcperf to produce a Chrome-trace JSON.
                         Requires admin rights AND VS dev shell.
                         On this system sudo is locked to --new-window,
                         so pass -Elevated to launch an elevated window.

    Results are saved to .opencode/profile/runs/<label>_<timestamp>/
    and can be analyzed with analyze.py or compared as baselines.

.PARAMETER Target
    Build target (EngineCore, EngineTests, etc). Default: EngineCore.

.PARAMETER Clean
    Switch. Perform a clean build (--clean-first). Default: true.

.PARAMETER Label
    Short descriptive name for this run (e.g. "post-refactor",
    "pre-optimization"). Auto-generated if omitted.

.PARAMETER Collect
    Profiling method: "btplus" (fast, no admin), "timetrace" (verbose, admin).

.PARAMETER Elevated
    Switch. Launch in a new elevated window for timetrace collection.
    Required when Collect=timetrace on this system.

.PARAMETER Jobs
    Parallel job count (Ninja -j flag). Default: 1 (serial for
    reproducible timing).

.PARAMETER OutDir
    Output directory for raw data. Default: .opencode/profile/runs/.

.EXAMPLE
    # Quick per-file timing (no admin needed)
    ./profile.ps1 -Target EngineCore -Label baseline

    # Full timetrace in elevated window
    ./profile.ps1 -Target EngineCore -Collect timetrace -Elevated
#>

param(
    [string]$Target = "EngineCore",
    [switch]$Clean = $true,
    [string]$Label = "",
    [ValidateSet("btplus", "timetrace")]
    [string]$Collect = "btplus",
    [switch]$Elevated = $false,
    [int]$Jobs = 1,
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

# --- paths ----------------------------------------------------------------
$repoRoot = Resolve-Path "$PSScriptRoot\..\..\.."
$buildDir = "$repoRoot\out\build\msvc-dev"
if (-not $OutDir) { $OutDir = "$repoRoot\.opencode\profile\runs" }

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
if (-not $Label) { $Label = "${Target}_${timestamp}" }

$runDir = "$OutDir\${Label}_${timestamp}"
New-Item -ItemType Directory -Path $runDir -Force | Out-Null

# --- elevated launch -----------------------------------------------------
if ($Elevated -and $Collect -eq "timetrace") {
    Write-Host "=== Launching elevated profiling window ==="
    $scriptPath = "$PSCommandPath"
    $argList = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", "`"$scriptPath`"",
        "-Target", "`"$Target`"",
        "-Collect", "timetrace",
        "-Label", "`"$Label`"",
        "-OutDir", "`"$OutDir`""
    )
    if ($Clean) { $argList += "-Clean" }
    if ($Jobs -ne 1) { $argList += "-Jobs", "$Jobs" }

    $startArgs = @{
        FilePath         = "pwsh.exe"
        ArgumentList     = $argList
        Verb             = "RunAs"
        WindowStyle      = "Normal"
        WorkingDirectory = $repoRoot
    }

    # The semaphore file signals the elevated process is done
    $doneFile = "$runDir\_done.txt"
    Start-Process @startArgs

    Write-Host "Elevated window launched for $Label. Waiting for completion..."
    while (-not (Test-Path $doneFile)) {
        Start-Sleep -Seconds 5
    }
    Write-Host "Elevated profiling complete. See $runDir"
    exit 0
}

# --- VS dev shell (for timetrace) ----------------------------------------
if ($Collect -eq "timetrace") {
    $vsPath = "C:\Program Files\Microsoft Visual Studio\18\Insiders"
    $devShellDll = "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    if (Test-Path $devShellDll) {
        try {
            Import-Module $devShellDll -Force
            Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
                -DevCmdArguments '-arch=x64 -host_arch=x64'
            Write-Host "VS dev shell loaded."
        } catch {
            Write-Warning "VS dev shell import failed. Falling back to PATH."
        }
    }

    # Verify vcperf is available
    $vcperf = Get-Command "vcperf.exe" -ErrorAction SilentlyContinue
    if (-not $vcperf) {
        Write-Error "vcperf.exe not found. Cannot collect timetrace without admin + VS dev shell."
        exit 1
    }

    # Stop lingering sessions
    vcperf /stop PPR_Core 2>$null
    Start-Sleep -Seconds 1
}

# --- build with /Bt+ ------------------------------------------------------
$btplusLog = "$runDir\btplus_output.txt"

Write-Host "=== Collection: $Collect | Target: $Target | Jobs: $Jobs ==="
Write-Host "Label: $Label"
Write-Host "Run dir: $runDir"
Write-Host ""

$originalCl = $env:CL
$env:CL = "/Bt+"

$buildArgs = @("--build", $buildDir, "--target", $Target)
if ($Clean) { $buildArgs += "--clean-first" }
if ($Jobs -gt 0) { $buildArgs += "--", "-j", "$Jobs" }

$elapsed = Measure-Command {
    if ($Collect -eq "timetrace") {
        Write-Host "Starting vcperf trace 'PPR_Core'..."
        vcperf /start PPR_Core /level verbose
        if ($LASTEXITCODE -ne 0) { throw "Failed to start vcperf" }
    }

    Write-Host "Building..."
    cmake @buildArgs 2>&1 | Tee-Object -FilePath $btplusLog | Out-Host
    $buildExit = $LASTEXITCODE
}

$env:CL = $originalCl

# --- stop timetrace -------------------------------------------------------
$timetraceJson = ""
if ($Collect -eq "timetrace") {
    $timetraceJson = "$runDir\timetrace.json"
    Write-Host "Stopping vcperf and exporting timetrace..."
    vcperf /stop PPR_Core /timetrace $timetraceJson 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "vcperf /stop returned exit code $LASTEXITCODE"
    }
}

# --- save metadata -------------------------------------------------------
$meta = @{
    label      = $Label
    target     = $Target
    clean      = $Clean.IsPresent
    jobs       = $Jobs
    collect    = $Collect
    build_ms   = [math]::Round($elapsed.TotalMilliseconds, 1)
    exit_code  = $buildExit
    timestamp  = (Get-Date).ToString("o")
}
$meta | ConvertTo-Json | Set-Content "$runDir\metadata.json"

# --- signal done (for elevated mode) ------------------------------------
if ($Elevated) {
    Set-Content "$runDir\_done.txt" "done"
}

Write-Host "=== Profile complete ==="
Write-Host "  Label:     $Label"
Write-Host "  Duration:  $($elapsed.TotalSeconds.ToString('F1')) s"
Write-Host "  Exit code: $buildExit"
Write-Host "  /Bt+ log:  $btplusLog"
if ($timetraceJson) {
    Write-Host "  Timetrace: $timetraceJson"
}
Write-Host ""
Write-Host "Next: analyze.py --btplus `"$btplusLog`" --label `"$Label`" [--save `"$Label`"]"
if ($timetraceJson) {
    Write-Host "      analyze.py --timetrace `"$timetraceJson`" --label `"$Label`" [--save `"$Label`"]"
}
