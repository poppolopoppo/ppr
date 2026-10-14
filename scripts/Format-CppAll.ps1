<#
.SYNOPSIS
    Parallel full-repository C++ reformatter for PPR.

.DESCRIPTION
    Thin, mechanical fan-out over scripts/Format-Cpp.ps1. It enumerates the
    same source set as `Format-Cpp.ps1 -All` (.cpp/.cppm/.h, same excluded
    directories) and runs one `Format-Cpp.ps1 -Mode Fix -Paths <file>`
    child process per file, concurrently.

    Parallelism uses the official PowerShell 7 `ForEach-Object -Parallel`
    runspace pool (no custom runspaces, jobs, or thread code), with
    `-ThrottleLimit` defaulting to the machine's logical processor count.
    Each child process owns its formatting, fixed-point iteration, token
    safety checks, and unique scratch directory, so concurrent children
    cannot share state; see Format-Cpp.ps1 NOTES. This script adds no
    formatting logic of its own.

    One child per file (rather than one child per batch of files) because
    `pwsh -File` binds only a single value to the child's `[string[]]$Paths`
    from the command line: passing several paths through one `-File` call
    silently formats only part of the batch (verified: 2 paths in, 1 file
    in scope, versus 2 in scope for the equivalent in-process call).

    On Windows PowerShell 5.1 (`ForEach-Object -Parallel` unavailable) it
    falls back to sequential processing with a warning. Exit codes match
    Format-Cpp.ps1: 0 complete, 1 incomplete (a child reported an unsafe,
    non-convergent, or otherwise unformatted file), 2 usage or internal
    error.

.PARAMETER ThrottleLimit
    Maximum concurrent formatter children. Defaults to the logical
    processor count.

.PARAMETER MaxPasses
    Passed through to each Format-Cpp.ps1 child. Upper bound on formatting
    passes used to reach a fixed point. Default 4.

.EXAMPLE
    pwsh -NoProfile -File scripts/Format-CppAll.ps1
    Reformat every C++ source using one child per logical processor.

.EXAMPLE
    pwsh -NoProfile -File scripts/Format-CppAll.ps1 -ThrottleLimit 8
    Cap parallelism at eight concurrent children.

.NOTES
    Requires pwsh 7+ for parallel execution and clang-format resolvable by
    Format-Cpp.ps1. The $SourceExtensions / $ExcludedDirRe filters must stay
    in sync with scripts/Format-Cpp.ps1; the formatter script remains the
    owner of all formatting semantics.
#>
[CmdletBinding()]
param(
    [ValidateRange(1, 128)]
    [int]$ThrottleLimit = [Environment]::ProcessorCount,

    [ValidateRange(1, 10)]
    [int]$MaxPasses = 4
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Must stay in sync with scripts/Format-Cpp.ps1. The gate covers .h files
# too, so a "cpp/cppm" reformat that skipped them would leave drift behind.
$SourceExtensions = @('.cpp', '.cppm', '.h')
$ExcludedDirRe = '(\\|/)(out|build|cmake-build[^\\/]*|Temp|Testing|_deps|vcpkg_installed|\.slim|\.idea|\.git|\.codegraph|\.windbg-symbol-cache)(\\|/|$)'
$RepoRoot = (Resolve-Path (Split-Path $PSScriptRoot -Parent)).Path
$FormatScript = Join-Path $PSScriptRoot 'Format-Cpp.ps1'

$exitCode = 2
try {
    if (-not (Test-Path -LiteralPath $FormatScript -PathType Leaf)) {
        Write-Error "Formatter script is missing: '$FormatScript'." -ErrorAction Continue
        $exitCode = 2
    }
    else {
        $files = @(Get-ChildItem -Path $RepoRoot -Recurse -File |
            Where-Object { $SourceExtensions -contains $_.Extension -and $_.FullName -notmatch $ExcludedDirRe } |
            ForEach-Object { $_.FullName } |
            Sort-Object -Unique)

        Write-Host "files: $($files.Count)   parallelism: $ThrottleLimit   max passes: $MaxPasses"
        Write-Host ''

        if ($files.Count -eq 0) {
            Write-Host 'No C++ sources found. Nothing to do.'
            $exitCode = 0
        }
        else {
            [string[]]$failed = @()

            if ($PSVersionTable.PSVersion.Major -ge 7 -and $files.Count -gt 1) {
                $failed = @($files | ForEach-Object -Parallel {
                        # Out-Host streams the child report to the console
                        # without feeding the success stream, so only
                        # failing paths below are collected into $failed.
                        & pwsh -NoProfile -File $using:FormatScript -Mode Fix -MaxPasses $using:MaxPasses -Paths $_ | Out-Host
                        if ($LASTEXITCODE -ne 0) {
                            $_
                        }
                    } -ThrottleLimit $ThrottleLimit)
            }
            else {
                if ($files.Count -gt 1) {
                    Write-Warning 'ForEach-Object -Parallel requires PowerShell 7+. Falling back to sequential processing.'
                }

                $failedList = @()
                foreach ($file in $files) {
                    & pwsh -NoProfile -File $FormatScript -Mode Fix -MaxPasses $MaxPasses -Paths $file
                    if ($LASTEXITCODE -ne 0) {
                        $failedList += $file
                    }
                }
                $failed = $failedList
            }

            Write-Host ''
            if ($failed.Count -gt 0) {
                Write-Host "Fix INCOMPLETE: $($failed.Count) of $($files.Count) file(s) reported failures."
                $exitCode = 1
            }
            else {
                Write-Host "Fix COMPLETE: $($files.Count) file(s) reformatted."
                $exitCode = 0
            }
        }
    }
}
catch {
    Write-Error "$_" -ErrorAction Continue
    $exitCode = 2
}
exit $exitCode
