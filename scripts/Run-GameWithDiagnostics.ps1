<#
.SYNOPSIS
    User-level game runner with WER crash-dump capture into out/crashes/.

.DESCRIPTION
    Runs the preset-built game/app.game.exe as a normal user, captures
    stdout/stderr plus the exit code into a per-run directory, and collects
    any matching WER LocalDumps dump from the inbox into that same directory.

    Run layout: out/crashes/<UTC-timestamp>-<PID>/ containing run-manifest.json,
    stdout.log, stderr.log, exit-code.txt, and any collected *.dmp.

    Provisioning is verified first via Setup-CrashDiagnostics.ps1 -Mode Check
    (fail fast when missing). Crash dumps are produced by WER, not by this
    script, so direct exe launches outside this runner still land in the
    inbox; -CollectPending gathers those orphaned dumps into run directories.

.PARAMETER Preset
    CMake preset whose build directory holds the game (default 'msvc-dev').
    Resolves to out/build/<Preset>/game/app.game.exe. Never builds; a missing
    exe is a hard error telling the operator to build first.

.PARAMETER CollectPending
    Instead of launching the game, collect inbox dumps that no run directory
    has claimed yet (e.g. from direct exe launches) into new pending run
    directories and exit.

.EXAMPLE
    pwsh -NoProfile -File scripts/Run-GameWithDiagnostics.ps1
    Run the msvc-dev game with diagnostics capture.

.EXAMPLE
    pwsh -NoProfile -File scripts/Run-GameWithDiagnostics.ps1 -Preset msvc-live -- --smoke
    Run the msvc-live game, passing --smoke through to the game.

.EXAMPLE
    pwsh -NoProfile -File scripts/Run-GameWithDiagnostics.ps1 -CollectPending
    Collect orphaned inbox dumps from direct exe launches.

.NOTES
    Stock pwsh 5.1 compatible. No modules. Non-interactive: never prompts,
    never pauses. Runs unelevated and never elevates; provisioning failures
    report the elevated Setup command instead. Never configures or builds.
    Exit codes: the game exit code is propagated on a launched run; 1 runner
    failure (provisioning, missing exe, dump-collection failure); 2 usage or
    internal error. `exit` sets the process exit code; do not dot-source.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'msvc-dev',

    [switch]$CollectPending,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$GameArgs = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Split-Path $PSScriptRoot -Parent)).Path
$CrashRoot = Join-Path $RepoRoot 'out\crashes'
$InboxDir = [Environment]::ExpandEnvironmentVariables('%LOCALAPPDATA%\PPR\CrashDumps\app.game.exe')
$DumpWaitSeconds = 60
Push-Location $RepoRoot

function Get-GitInfo {
    $rev = (& git rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($rev)) { $rev = 'unknown' }
    else { $rev = $rev.Trim() }
    $porcelain = (& git status --porcelain 2>$null)
    $dirty = $false
    if ($LASTEXITCODE -eq 0 -and $porcelain) { $dirty = $true }
    return [pscustomobject]@{ Revision = $rev; Dirty = $dirty }
}

# Redact secret-looking values so argv can be stored in the manifest.
# Keeps the flag name, masks the value: --token=abc -> --token=***,
# --password secret -> --password *** (next element masked).
function Sanitize-Argv {
    param([string[]]$Argv)
    $secretRe = '^--?(token|secret|password|passwd|api[-_]?key|auth|bearer)([=:]|$)'
    $out = @()
    $maskNext = $false
    foreach ($a in $Argv) {
        if ($maskNext) {
            $out += '***'
            $maskNext = $false
            continue
        }
        if ($a -match $secretRe) {
            if ($a -match '[=:]') {
                $out += ($a -replace '([=:]).*$', '$1***')
            }
            else {
                $out += $a
                $maskNext = $true
            }
        }
        else {
            $out += $a
        }
    }
    if ($maskNext) { $out += '***' }
    return $out
}

function Write-Manifest {
    param([string]$Path, [pscustomobject]$Manifest)
    $Manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

# Bounded wait until WER has closed the dump file (readable exclusively),
# then copy it into the run directory. Returns the destination path.
function Wait-AndCollectDump {
    param([string]$Source, [string]$RunDir)

    $deadline = (Get-Date).AddSeconds($DumpWaitSeconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $stream = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
            $stream.Close()
            break
        }
        catch {
            Start-Sleep -Milliseconds 500
        }
    }
    try {
        $probe = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
        $probe.Close()
    }
    catch {
        throw "Timed out waiting for WER to close '$Source' after $DumpWaitSeconds seconds."
    }
    $dest = Join-Path $RunDir (Split-Path $Source -Leaf)
    Copy-Item -LiteralPath $Source -Destination $dest -Force
    return $dest
}

function Get-CollectedDumpNames {
    $names = @{}
    if (-not (Test-Path -LiteralPath $CrashRoot)) { return $names }
    foreach ($manifestFile in (Get-ChildItem -Path $CrashRoot -Recurse -Filter 'run-manifest.json' -ErrorAction SilentlyContinue)) {
        try {
            $m = Get-Content -LiteralPath $manifestFile.FullName -Raw | ConvertFrom-Json
            foreach ($d in @($m.collectedDumps)) {
                if ($d -and $d.sourceFile) { $names["$($d.sourceFile)"] = $true }
            }
        }
        catch {
            continue
        }
    }
    foreach ($dumpFile in (Get-ChildItem -Path $CrashRoot -Recurse -Filter '*.dmp' -ErrorAction SilentlyContinue)) {
        $names[$dumpFile.Name] = $true
    }
    return $names
}

$exitCode = 2
try {
    if (-not (Test-Path -LiteralPath $CrashRoot)) {
        New-Item -ItemType Directory -Force -Path $CrashRoot | Out-Null
    }

    if ($CollectPending) {
        if (-not (Test-Path -LiteralPath $InboxDir)) {
            Write-Host 'No inbox directory; nothing pending.'
            $exitCode = 0
        }
        else {
            $claimed = Get-CollectedDumpNames
            $pending = @(Get-ChildItem -Path $InboxDir -Filter '*.dmp' -ErrorAction SilentlyContinue |
                Where-Object { -not $claimed.ContainsKey($_.Name) } |
                Sort-Object LastWriteTime)
            if ($pending.Count -eq 0) {
                Write-Host 'No pending dumps in inbox.'
                $exitCode = 0
            }
            else {
                foreach ($dump in $pending) {
                    $stamp = $dump.LastWriteTime.ToUniversalTime().ToString('yyyyMMdd-HHmmss')
                    $runDir = Join-Path $CrashRoot ("{0}-pending" -f $stamp)
                    $suffix = 1
                    while (Test-Path -LiteralPath $runDir) {
                        $suffix++
                        $runDir = Join-Path $CrashRoot ("{0}-pending-{1}" -f $stamp, $suffix)
                    }
                    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
                    $dest = Wait-AndCollectDump -Source $dump.FullName -RunDir $runDir
                    $git = Get-GitInfo
                    Write-Manifest -Path (Join-Path $runDir 'run-manifest.json') -Manifest ([pscustomobject]@{
                        schemaVersion      = 1
                        utc                = (Get-Date).ToUniversalTime().ToString('o')
                        directLaunch       = $true
                        pid                = $null
                        processCreationTime = $null
                        exePath            = $null
                        preset             = $null
                        repoRoot           = $RepoRoot
                        gitRevision        = $git.Revision
                        gitDirty           = $git.Dirty
                        argv               = @()
                        exitCode           = $null
                        collectedDumps     = @([pscustomobject]@{ sourceFile = $dump.Name; runFile = (Split-Path $dest -Leaf) })
                    })
                    Write-Host "Collected pending dump '$($dump.Name)' into '$runDir'."
                }
                $exitCode = 0
            }
        }
    }
    else {
        $exe = Join-Path $RepoRoot ("out\build\{0}\game\app.game.exe" -f $Preset)
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            Write-Error ("Game exe not found: '$exe'. Build it first (e.g. cmake --preset $Preset), " +
                'this runner never builds.') -ErrorAction Continue
            $exitCode = 2
        }
        else {
            & (Join-Path $PSScriptRoot 'Setup-CrashDiagnostics.ps1') -Mode Check
            if ($LASTEXITCODE -ne 0) {
                Write-Error ('Crash-diagnostics provisioning check failed. Provision first: ' +
                    "Start-Process pwsh -Verb RunAs -ArgumentList '-NoProfile -File scripts/Setup-CrashDiagnostics.ps1 -Mode Install'.") -ErrorAction Continue
                $exitCode = 1
            }
            else {
                $utc = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
                $stageDir = Join-Path $CrashRoot ("run-{0}-staging" -f $utc)
                New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
                $stdoutLog = Join-Path $stageDir 'stdout.log'
                $stderrLog = Join-Path $stageDir 'stderr.log'

                Write-Host "Launching '$exe' (preset $Preset)..."
                $proc = Start-Process -FilePath $exe -ArgumentList $GameArgs `
                    -NoNewWindow -PassThru -Wait `
                    -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
                $gameCode = $proc.ExitCode
                $gamePid = $proc.Id
                $creationTime = $proc.StartTime.ToUniversalTime().ToString('o')

                $runDir = Join-Path $CrashRoot ("{0}-{1}" -f $utc, $gamePid)
                $suffix = 1
                while (Test-Path -LiteralPath $runDir) {
                    $suffix++
                    $runDir = Join-Path $CrashRoot ("{0}-{1}-{2}" -f $utc, $gamePid, $suffix)
                }
                Move-Item -LiteralPath $stageDir -Destination $runDir -Force
                Set-Content -LiteralPath (Join-Path $runDir 'exit-code.txt') -Value "$gameCode" -Encoding UTF8

                $git = Get-GitInfo
                $collected = @()
                if (Test-Path -LiteralPath $InboxDir) {
                    $windowStart = $proc.StartTime.AddMinutes(-1)
                    $candidates = @(Get-ChildItem -Path $InboxDir -Filter '*.dmp' -ErrorAction SilentlyContinue |
                        Where-Object { $_.LastWriteTime -ge $windowStart } |
                        Sort-Object LastWriteTime)
                    foreach ($dump in $candidates) {
                        $dest = Wait-AndCollectDump -Source $dump.FullName -RunDir $runDir
                        $collected += [pscustomobject]@{ sourceFile = $dump.Name; runFile = (Split-Path $dest -Leaf) }
                        Write-Host "Collected dump '$($dump.Name)'."
                    }
                }

                Write-Manifest -Path (Join-Path $runDir 'run-manifest.json') -Manifest ([pscustomobject]@{
                    schemaVersion       = 1
                    utc                 = $utc
                    directLaunch        = $false
                    pid                 = $gamePid
                    processCreationTime = $creationTime
                    exePath             = $exe
                    preset              = $Preset
                    repoRoot            = $RepoRoot
                    gitRevision         = $git.Revision
                    gitDirty            = $git.Dirty
                    argv                = @(Sanitize-Argv $GameArgs)
                    exitCode            = $gameCode
                    collectedDumps      = $collected
                })
                Write-Host "Run captured in '$runDir' (exit $gameCode, $($collected.Count) dump(s))."
                $exitCode = $gameCode
            }
        }
    }
}
catch {
    Write-Error "$_" -ErrorAction Continue
    $exitCode = 2
}
finally {
    Pop-Location
}
exit $exitCode
