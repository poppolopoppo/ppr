<#
.SYNOPSIS
    Triages a captured game crash dump with windbg-tool into its run directory.

.DESCRIPTION
    Analyzes the dump collected for one out/crashes/ run: reads run-manifest.json
    (plus build-manifest.json beside the exe or in the build directory when a
    future build integration emits one) to derive the dump path, image path,
    source root, and symbol cache -- never from operator input -- then sets
    _NT_SYMBOL_PATH / _NT_SOURCE_PATH, runs `windbg-tool discover` followed by
    `windbg-tool --compact --envelope dump triage <dump> --max-frames 64
    --image-path <images> --symbol-cache <cache>`, and writes triage.json plus
    a human-readable triage-summary.md into the run directory (raw envelope
    bytes stage in analysis-scratch/ first, then validate, then publish).

    Missing symbols or a PDB mismatch fail visibly (non-zero): the summary
    records the failure and no empty success report is ever written.

.PARAMETER RunDir
    Explicit run directory (e.g. out/crashes/20261008-120000-1234). Exactly one
    of -RunDir / -Pending is required.

.PARAMETER Pending
    Analyze the newest unanalyzed run (has run-manifest.json and a collected
    dump, but no triage.json yet).

.EXAMPLE
    pwsh -NoProfile -File scripts/Invoke-CrashAnalysis.ps1 -Pending

.EXAMPLE
    pwsh -NoProfile -File scripts/Invoke-CrashAnalysis.ps1 -RunDir out/crashes/20261008-120000-1234

.NOTES
    Stock pwsh 5.1 compatible. No modules. Non-interactive: never prompts,
    never pauses. Runs unelevated and never elevates. Never configures or
    builds. Exit codes: 0 triage succeeded, 1 triage/symbol failure or no
    dump to analyze, 2 usage or internal error.
    `exit` sets the process exit code; do not dot-source this file.
#>
[CmdletBinding()]
param(
    [string]$RunDir = '',

    [switch]$Pending
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Split-Path $PSScriptRoot -Parent)).Path
$CrashRoot = Join-Path $RepoRoot 'out\crashes'
$SymbolCache = Join-Path $CrashRoot 'symbol-cache'
$Msdl = 'https://msdl.microsoft.com/download/symbols'
Push-Location $RepoRoot

function Find-NewestUnanalyzed {
    if (-not (Test-Path -LiteralPath $CrashRoot)) { return $null }
    $best = $null
    foreach ($manifestFile in (Get-ChildItem -Path $CrashRoot -Recurse -Filter 'run-manifest.json' -ErrorAction SilentlyContinue)) {
        $dir = Split-Path $manifestFile.FullName -Parent
        if (Test-Path -LiteralPath (Join-Path $dir 'triage.json')) { continue }
        if (@(Get-ChildItem -Path $dir -Filter '*.dmp' -ErrorAction SilentlyContinue).Count -eq 0) { continue }
        if (($null -eq $best) -or ($dir -gt $best)) { $best = $dir }
    }
    return $best
}

function Read-BuildManifest {
    param([string]$ExePath, [string]$Preset)
    foreach ($candidate in @(
        (Join-Path (Split-Path $ExePath -Parent) 'build-manifest.json'),
        (Join-Path $RepoRoot ("out\build\{0}\build-manifest.json" -f $Preset))
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            try {
                return (Get-Content -LiteralPath $candidate -Raw | ConvertFrom-Json)
            }
            catch {
                Write-Host "WARNING: ignoring unreadable build manifest '$candidate': $($_.Exception.Message)"
            }
        }
    }
    return $null
}

function Write-Summary {
    param(
        [string]$Path,
        [string]$RunId,
        [string]$Dump,
        [string]$Exe,
        [string]$Preset,
        [string]$Revision,
        [string]$Command,
        [string]$Outcome,
        [object]$Triage
    )
    $lines = New-Object System.Collections.ArrayList
    [void]$lines.Add("# Crash triage: $RunId")
    [void]$lines.Add('')
    [void]$lines.Add("- Dump: ``$Dump``")
    [void]$lines.Add("- Exe: ``$Exe`` (preset $Preset, rev $Revision)")
    [void]$lines.Add("- Command: ``$Command``")
    [void]$lines.Add("- Outcome: $Outcome")
    [void]$lines.Add('')
    if ($null -ne $Triage) {
        foreach ($propName in @('exception', 'exceptionRecord', 'crash', 'error', 'errors', 'status', 'process', 'image')) {
            $value = $Triage.$propName
            if ($null -ne $value) {
                $text = ($value | ConvertTo-Json -Depth 6 -Compress)
                [void]$lines.Add("## $propName")
                [void]$lines.Add('')
                [void]$lines.Add('```')
                [void]$lines.Add($text)
                [void]$lines.Add('```')
                [void]$lines.Add('')
            }
        }
        $frames = $Triage.frames
        if ($null -eq $frames) {
            foreach ($thread in @($Triage.threads)) {
                if ($thread -and $thread.frames) { $frames = $thread.frames; break }
            }
        }
        if ($frames) {
            [void]$lines.Add('## Top frames')
            [void]$lines.Add('')
            $index = 0
            foreach ($frame in @($frames)) {
                $text = ($frame | ConvertTo-Json -Depth 4 -Compress)
                [void]$lines.Add(("{0}: {1}" -f $index, $text))
                $index++
                if ($index -ge 64) { break }
            }
            [void]$lines.Add('')
        }
        $modules = @($Triage.modules)
        if ($modules.Count -gt 0) {
            [void]$lines.Add('## Modules / symbols')
            [void]$lines.Add('')
            foreach ($mod in $modules) {
                [void]$lines.Add(('- ' + ($mod | ConvertTo-Json -Depth 4 -Compress)))
            }
            [void]$lines.Add('')
        }
    }
    else {
        [void]$lines.Add('No triage JSON was produced; see triage.stderr.log for tool output.')
        [void]$lines.Add('')
    }
    [void]$lines.Add('Full machine-readable result: triage.json in this directory.')
    $lines | Set-Content -LiteralPath $Path -Encoding UTF8
}

$exitCode = 2
try {
    if ((-not [string]::IsNullOrWhiteSpace($RunDir)) -and $Pending) {
        Write-Error 'Pass exactly one of -RunDir / -Pending.' -ErrorAction Continue
        $exitCode = 2
    }
    else {
        if ($Pending) {
            $RunDir = Find-NewestUnanalyzed
            if ([string]::IsNullOrWhiteSpace($RunDir)) {
                Write-Error 'No unanalyzed run with a collected dump under out/crashes/.' -ErrorAction Continue
                $exitCode = 1
            }
        }
        elseif ([string]::IsNullOrWhiteSpace($RunDir)) {
            Write-Error 'Pass exactly one of -RunDir / -Pending.' -ErrorAction Continue
            $exitCode = 2
        }
        elseif (-not (Test-Path -LiteralPath $RunDir -PathType Container)) {
            Write-Error "Run directory not found: '$RunDir'." -ErrorAction Continue
            $exitCode = 2
        }

        if ($exitCode -ne 2 -and -not [string]::IsNullOrWhiteSpace($RunDir)) {
            $RunDir = (Resolve-Path -LiteralPath $RunDir).Path
            $runId = Split-Path $RunDir -Leaf
            $manifestPath = Join-Path $RunDir 'run-manifest.json'
            if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
                Write-Error "Run manifest missing: '$manifestPath'. Collect the run with scripts/Run-GameWithDiagnostics.ps1 first." -ErrorAction Continue
                $exitCode = 2
            }
            else {
                $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

                # --- Derive dump / images / sources / cache (never operator input)
                $dump = $null
                foreach ($entry in @($manifest.collectedDumps)) {
                    if ($entry -and $entry.runFile) {
                        $candidate = Join-Path $RunDir "$($entry.runFile)"
                        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $dump = $candidate; break }
                    }
                }
                if ($null -eq $dump) {
                    $first = @(Get-ChildItem -Path $RunDir -Filter '*.dmp' -ErrorAction SilentlyContinue |
                        Sort-Object LastWriteTime | Select-Object -First 1)
                    if ($first.Count -gt 0) { $dump = $first[0].FullName }
                }
                if ($null -eq $dump) {
                    Write-Summary -Path (Join-Path $RunDir 'triage-summary.md') -RunId $runId `
                        -Dump '<none>' -Exe "$($manifest.exePath)" -Preset "$($manifest.preset)" `
                        -Revision "$($manifest.gitRevision)" -Command '<not run>' `
                        -Outcome 'FAILED: no dump was collected for this run (the process may not have crashed).' -Triage $null
                    Write-Error "No dump to analyze in '$RunDir' (no crash captured). See triage-summary.md." -ErrorAction Continue
                    $exitCode = 1
                }
                else {
                    $buildInfo = $null
                    if ($manifest.exePath) {
                        $buildInfo = Read-BuildManifest -ExePath "$($manifest.exePath)" -Preset "$($manifest.preset)"
                    }
                    $images = Split-Path "$($manifest.exePath)" -Parent
                    $sources = "$($manifest.repoRoot)"
                    if ($buildInfo -and $buildInfo.imageDir) { $images = "$($buildInfo.imageDir)" }
                    if ($buildInfo -and $buildInfo.sourceRoot) { $sources = "$($buildInfo.sourceRoot)" }
                    if ([string]::IsNullOrWhiteSpace($sources)) { $sources = $RepoRoot }

                    $exeLeaf = Split-Path "$($manifest.exePath)" -Leaf
                    $pdbLeaf = [IO.Path]::ChangeExtension($exeLeaf, '.pdb')
                    $pdbPath = Join-Path $images $pdbLeaf
                    if (-not (Test-Path -LiteralPath $pdbPath -PathType Leaf)) {
                        Write-Summary -Path (Join-Path $RunDir 'triage-summary.md') -RunId $runId `
                            -Dump $dump -Exe "$($manifest.exePath)" -Preset "$($manifest.preset)" `
                            -Revision "$($manifest.gitRevision)" -Command '<not run>' `
                            -Outcome ("FAILED: symbols missing -- PDB not found at '$pdbPath'. " +
                            'Rebuild the preset so PDBs sit beside the exe, then re-run analysis.') -Triage $null
                        Write-Error "Missing symbols: PDB not found at '$pdbPath'." -ErrorAction Continue
                        $exitCode = 1
                    }
                    else {
                        if (-not (Test-Path -LiteralPath $SymbolCache)) {
                            New-Item -ItemType Directory -Force -Path $SymbolCache | Out-Null
                        }
                        $env:_NT_SYMBOL_PATH = "srv*$SymbolCache*$Msdl;$images"
                        $env:_NT_SOURCE_PATH = $sources

                        if (-not (Get-Command windbg-tool -ErrorAction SilentlyContinue)) {
                            Write-Error 'windbg-tool not found on PATH (dotnet tool install -g Devolutions.WinDbg.Tool).' -ErrorAction Continue
                            $exitCode = 2
                        }
                        else {
                            & windbg-tool discover
                            if ($LASTEXITCODE -ne 0) {
                                Write-Error "windbg-tool discover failed with exit code $LASTEXITCODE; fix the install/daemon first." -ErrorAction Continue
                                $exitCode = 1
                            }
                            else {
                                $scratch = Join-Path $RunDir 'analysis-scratch'
                                if (-not (Test-Path -LiteralPath $scratch)) {
                                    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
                                }
                                $rawPath = Join-Path $scratch 'triage.raw.json'
                                $stderrLog = Join-Path $RunDir 'triage.stderr.log'
                                $triagePath = Join-Path $RunDir 'triage.json'
                                if (Test-Path -LiteralPath $triagePath) { Remove-Item -LiteralPath $triagePath -Force }

                                $triageCmd = "windbg-tool --compact --envelope dump triage `"$dump`" --max-frames 64 --image-path `"$images`" --symbol-cache `"$SymbolCache`""
                                $toolOut = & windbg-tool --compact --envelope dump triage $dump --max-frames 64 --image-path $images --symbol-cache $SymbolCache 2> $stderrLog
                                $toolCode = $LASTEXITCODE
                                if ($null -ne $toolOut) {
                                    $toolOut | Set-Content -LiteralPath $rawPath -Encoding UTF8
                                }

                                $failure = $null
                                $triage = $null
                                if ($toolCode -ne 0) {
                                    $failure = "windbg-tool dump triage exited with code $toolCode; see triage.stderr.log."
                                }
                                elseif (-not (Test-Path -LiteralPath $rawPath -PathType Leaf) -or
                                        ((Get-Item -LiteralPath $rawPath).Length -eq 0)) {
                                    $failure = 'windbg-tool produced no output; refusing to write an empty success report.'
                                }
                                else {
                                    $rawText = Get-Content -LiteralPath $rawPath -Raw
                                    try {
                                        $triage = $rawText | ConvertFrom-Json
                                    }
                                    catch {
                                        $failure = 'Triage output is not valid JSON; see analysis-scratch/triage.raw.json.'
                                    }
                                    if (($null -eq $failure) -and ($rawText -match '(?i)pdb.{0,20}mismatch|symbols?\s+could\s+not\s+(be\s+)?loaded|failed\s+to\s+load\s+symbols|symbol\s+file\s+not\s+found')) {
                                        $failure = 'Symbol failure reported (missing symbols or PDB mismatch); see triage output.'
                                    }
                                    if ($null -eq $failure) {
                                        $status = "$($triage.status)"
                                        if ($status -match '(?i)^(error|failed|failure)$') {
                                            $failure = "Triage reported status '$status'."
                                        }
                                        elseif (($null -ne $triage.errors) -and (@($triage.errors).Count -gt 0)) {
                                            $failure = 'Triage reported errors: ' + (($triage.errors | ConvertTo-Json -Depth 4 -Compress))
                                        }
                                    }
                                }

                                if ($null -eq $failure) {
                                    Copy-Item -LiteralPath $rawPath -Destination $triagePath -Force
                                    Write-Summary -Path (Join-Path $RunDir 'triage-summary.md') -RunId $runId `
                                        -Dump $dump -Exe "$($manifest.exePath)" -Preset "$($manifest.preset)" `
                                        -Revision "$($manifest.gitRevision)" -Command $triageCmd `
                                        -Outcome 'SUCCESS: triage completed; frames/symbols recorded below.' -Triage $triage
                                    Write-Host "Triage complete: '$triagePath' + triage-summary.md."
                                    $exitCode = 0
                                }
                                else {
                                    if (($null -ne $triage) -or (Test-Path -LiteralPath $rawPath)) {
                                        if ($null -eq $triage -and (Test-Path -LiteralPath $rawPath)) {
                                            Copy-Item -LiteralPath $rawPath -Destination $triagePath -Force
                                        }
                                        elseif ($null -ne $triage) {
                                            $triage | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $triagePath -Encoding UTF8
                                        }
                                    }
                                    Write-Summary -Path (Join-Path $RunDir 'triage-summary.md') -RunId $runId `
                                        -Dump $dump -Exe "$($manifest.exePath)" -Preset "$($manifest.preset)" `
                                        -Revision "$($manifest.gitRevision)" -Command $triageCmd `
                                        -Outcome ("FAILED: $failure") -Triage $triage
                                    Write-Error $failure -ErrorAction Continue
                                    $exitCode = 1
                                }
                            }
                        }
                    }
                }
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
