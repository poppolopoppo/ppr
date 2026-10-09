<##
.SYNOPSIS
    Prepares a captured game run for analysis through official WinDbg MCP.
.DESCRIPTION
    Resolves a dump, original image directory, source root, and symbol cache and
    writes analysis-request.json. Does not open WinDbg, analyze memory, or claim
    a triage result. Existing triage reports are preserved.
.NOTES
    Exit 0: request prepared; 1: no dump/pending run; 2: usage/manifest failure.
    Compatible with Windows PowerShell 5.1. Do not dot-source.
#>
[CmdletBinding()]
param([string]$RunDir = '', [switch]$Pending)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = (Resolve-Path (Split-Path $PSScriptRoot -Parent)).Path
$crashRoot = Join-Path $repoRoot 'out\crashes'
try {
    if ($Pending -eq (-not [string]::IsNullOrWhiteSpace($RunDir))) {
        throw 'Pass exactly one of -RunDir / -Pending.'
    }
    if ($Pending) {
        $runs = @()
        if (Test-Path -LiteralPath $crashRoot -PathType Container) {
            $runs = @(Get-ChildItem -LiteralPath $crashRoot -Directory | Sort-Object Name -Descending)
        }
        foreach ($run in $runs) {
            if (-not (Test-Path -LiteralPath (Join-Path $run.FullName 'run-manifest.json'))) { continue }
            if (Test-Path -LiteralPath (Join-Path $run.FullName 'triage-summary.md')) { continue }
            if (Test-Path -LiteralPath (Join-Path $run.FullName 'triage.json')) { continue }
            if (@(Get-ChildItem -LiteralPath $run.FullName -Filter '*.dmp' -File).Count -eq 0) { continue }
            $RunDir = $run.FullName
            break
        }
        if ([string]::IsNullOrWhiteSpace($RunDir)) {
            [Console]::Error.WriteLine('No pending run with a collected dump under out/crashes.')
            exit 1
        }
    }
    if (-not [IO.Path]::IsPathRooted($RunDir)) { $RunDir = Join-Path $repoRoot $RunDir }
    $RunDir = (Resolve-Path -LiteralPath $RunDir).Path
    $manifest = Get-Content -LiteralPath (Join-Path $RunDir 'run-manifest.json') -Raw | ConvertFrom-Json
    $dumps = @(Get-ChildItem -LiteralPath $RunDir -Filter '*.dmp' -File | Sort-Object Name)
    if ($dumps.Count -eq 0) {
        [Console]::Error.WriteLine('No collected dump in the selected run. Preparation did not analyze anything.')
        exit 1
    }
    $images = $null
    $exe = $null
    if ($manifest.PSObject.Properties['exePath'] -and $manifest.exePath) {
        $exe = [string]$manifest.exePath
        $images = Split-Path $exe -Parent
    }
    $sources = $repoRoot
    if ($manifest.PSObject.Properties['repoRoot'] -and $manifest.repoRoot) { $sources = [string]$manifest.repoRoot }
    $cache = Join-Path $crashRoot 'symbol-cache'
    $symbolPath = 'srv*' + $cache + '*https://msdl.microsoft.com/download/symbols'
    if ($images) { $symbolPath = $images + ';' + $symbolPath }
    $request = [ordered]@{
        schemaVersion = 1
        status = 'prepared_not_analyzed'
        server = 'WinDbg'
        runDirectory = $RunDir
        manifestPath = Join-Path $RunDir 'run-manifest.json'
        dumps = @($dumps | ForEach-Object { $_.FullName })
        originalExecutable = $exe
        imageDirectory = $images
        sourceRoot = $sources
        symbolCache = $cache
        symbolPath = $symbolPath
        reportPath = Join-Path $RunDir 'triage-summary.md'
        instructions = @(
            'Start MCP Service in an empty WinDbg window, then select that session through WinDbg MCP.',
            'Verify get_target_state; choose the intended dump explicitly if more than one is listed.',
            'Use open_dump only with no active target; configure symbols/source with show_output.',
            'Verify the original image/PDB identity; preparation does not validate symbols.',
            'Investigate and save evidence, alternatives, confidence, and gaps in a report without replacing an existing report.'
        )
    }
    $path = Join-Path $RunDir 'analysis-request.json'
    $request | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding UTF8
    Write-Output "Prepared: $path"
    Write-Output 'Analysis is pending in official WinDbg MCP; no triage result was produced.'
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 2
}
