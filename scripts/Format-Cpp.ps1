<#
.SYNOPSIS
    Deterministic C++ formatting gate for PPR. The single formatter entry point
    for humans, agents, and CI.

.DESCRIPTION
    Runs clang-format over the changed ranges plus any surrounding syntax the
    formatter needs to parse them, and enforces machine-checkable invariants.
    It never asks for an opinion. The script copies the whole formatted result
    back, so "changed lines only" is a scope, not a guarantee that untouched
    bytes are never rewritten.

    Invariants enforced:
      1. Token safety - the case-sensitive code token stream, including string
                         and char literal contents, is unchanged, so formatting
                         cannot silently alter code.
      2. Fixed point   - repeated formatting reaches the same bytes within a
                         bounded number of passes.
      3. No drift      - in Check mode the working tree already holds the
                         canonical output.

    Formatting is applied to a scratch copy of each real file rather than to
    piped stdin, so Check mode stays read-only and a failure never leaves a
    half-written file behind. The style is passed explicitly by absolute path
    to the repository .clang-format, so style resolution cannot depend on where
    the scratch copy happens to live.

    clang-format is known to need more than one pass on some files
    (llvm/llvm-project#52929), so both modes iterate to a fixed point rather than
    assuming one pass is canonical. The pass budget is bounded, so the result
    stays deterministic.

    Why changed-range scoping: this repository's historical formatting came from
    CLion with KEEP_USER_LINEBREAKS=true, which preserves author line breaks.
    clang-format has no equivalent. Scoping to changed ranges keeps legacy
    formatting untouched while making every new line canonical. Note that
    ColumnLimit 0 only disables column-driven wrapping; structural rules still
    normalize some line breaks (for example collapsing a trivial function body
    onto one line), which is why the safety checks below exist.

.PARAMETER Mode
    Check (default) reports drift and exits 1 if any selected line is not
    canonically formatted. Fix rewrites the selected lines in place.

.PARAMETER Base
    Git revision the change set is measured against. Defaults to HEAD, which
    selects unstaged plus staged changes on tracked files. CI should pass an
    explicit merge-base commit.

.PARAMETER All
    Opt-in full-repository mode. Off by default because it rewrites untouched
    code; use only as a deliberate, dedicated formatting-only change.

.PARAMETER Paths
    Optional explicit file or directory list, formatted in full. Overrides
    Base-driven selection.

.PARAMETER MaxPasses
    Upper bound on formatting passes used to reach a fixed point. Default 4.

.EXAMPLE
    pwsh -NoProfile -File scripts/Format-Cpp.ps1
    Check the working tree against HEAD.

.EXAMPLE
    pwsh -NoProfile -File scripts/Format-Cpp.ps1 -Mode Fix
    Format the changed lines in place.

.EXAMPLE
    pwsh -NoProfile -File scripts/Format-Cpp.ps1 -Base origin/main
    Check only the lines that differ from origin/main.

.NOTES
    Requires clang-format on PATH or in a standard LLVM install.
    Exit codes: 0 pass, 1 gate failure (drift, token-unsafe, non-convergent),
    2 usage or internal error (missing style, unknown path, tool failure).
    Read-only in Check mode. Each invocation formats scratch copies in its own
    unique directory under .slim/tmp/format-work/run-<guid>, which is gitignored
    and removed in a finally block, so concurrent runs cannot share files.
    Include reordering and string-literal breaking are pinned off in
    .clang-format so the formatter cannot change code meaning.
#>
[CmdletBinding()]
param(
    [ValidateSet('Check', 'Fix')]
    [string]$Mode = 'Check',

    [string]$Base = 'HEAD',

    [switch]$All,

    [string[]]$Paths,

    [ValidateRange(1, 10)]
    [int]$MaxPasses = 4
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$SourceExtensions = @('.cpp', '.cppm', '.h')
$ExcludedDirRe = '(\\|/)(out|build|cmake-build[^\\/]*|Temp|Testing|_deps|vcpkg_installed|\.slim|\.idea|\.git|\.codegraph|\.windbg-symbol-cache)(\\|/|$)'
$RepoRoot = (Resolve-Path (Split-Path $PSScriptRoot -Parent)).Path
$StyleFile = Join-Path $RepoRoot '.clang-format'
$ScratchBase = Join-Path $RepoRoot '.slim\tmp\format-work'
$ScratchDir = Join-Path $ScratchBase ('run-' + [guid]::NewGuid().ToString('N'))
Push-Location $RepoRoot

function Resolve-ClangFormat {
    $cmd = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidates = @(
        "$env:ProgramFiles\LLVM\bin\clang-format.exe",
        "$env:LOCALAPPDATA\Programs\LLVM\bin\clang-format.exe"
    ) | Where-Object { Test-Path -LiteralPath $_ }

    if ($candidates) { return @($candidates)[0] }
    throw 'clang-format not found. Install LLVM or add clang-format to PATH.'
}

function Get-RepoRelative {
    param([string]$FullPath)
    $prefix = $RepoRoot + [IO.Path]::DirectorySeparatorChar
    if ($FullPath.StartsWith($prefix)) { return $FullPath.Substring($prefix.Length) }
    $altPrefix = $RepoRoot + '/'
    if ($FullPath.StartsWith($altPrefix)) { return $FullPath.Substring($altPrefix.Length) }
    return $FullPath
}

# Only line endings are normalized here. Trailing newlines are significant:
# end-of-file and blank-line-limit violations must compare as drift.
function Normalize-Text {
    param([string]$Text)
    return ($Text -replace "`r`n", "`n" -replace "`r", "`n")
}

# Contiguous changed ranges in the NEW file as "start:end". An empty result
# means the file has no changes against the base revision.
function Get-ChangedRanges {
    param([string]$FullPath, [string]$Revision)

    $diff = & git diff -U0 --no-color $Revision -- (Get-RepoRelative $FullPath) 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "git diff failed with exit code $LASTEXITCODE for '$(Get-RepoRelative $FullPath)'."
    }
    if (-not $diff) { return @() }

    $ranges = @()
    foreach ($line in $diff) {
        if ($line -notmatch '^@@ -\S+ \+(\d+)(?:,(\d+))? @@') { continue }

        $start = [int]$Matches[1]
        $count = if ($Matches.ContainsKey(2) -and $Matches[2]) { [int]$Matches[2] } else { 1 }
        if ($count -gt 0) {
            # git hunk offsets and clang-format --lines are both 1-based.
            $ranges += "${start}:$(($start + $count) - 1)"
        }
    }
    return $ranges
}

# Format the file in place until its bytes stop changing, or the budget runs
# out. Returns whether it settled and how many passes it took. A non-zero
# clang-format exit is an internal error, never a clean convergence.
function Invoke-FormatToFixedPoint {
    param([string]$Path, [string[]]$Ranges, [string]$Formatter, [string]$StyleArg, [int]$Budget)

    $previous = Normalize-Text (Get-Content -LiteralPath $Path -Raw)
    for ($pass = 1; $pass -le $Budget; $pass++) {
        $clangArgs = @($StyleArg, '-i')
        foreach ($r in $Ranges) {
            $clangArgs += @('--lines', $r)
        }
        $clangArgs += $Path
        & $Formatter @clangArgs
        if ($LASTEXITCODE -ne 0) {
            throw "clang-format failed with exit code $LASTEXITCODE on '$Path'."
        }

        $current = Normalize-Text (Get-Content -LiteralPath $Path -Raw)
        if ($current -ceq $previous) {
            return [pscustomobject]@{ Converged = $true; Passes = $pass }
        }
        $previous = $current
    }
    return [pscustomobject]@{ Converged = $false; Passes = $Budget }
}

# The formatter contract is whitespace- and comment-layout-only. Literals are
# masked with opaque placeholders before anything is stripped, so comment
# markers and whitespace inside a literal (including "//" in a URL and spaces
# inside "a b") survive comparison verbatim. Comment and whitespace stripping
# then applies only outside literals, and whitespace collapses to a single
# space so token boundaries stay significant ("int x;" differs from "intx;").
# Line splices are removed only as backslash-plus-line-terminator and only
# outside literals, so a backslash inside "\n" is never confused with one.
# Raw strings are masked too; a leftover raw-string opener means the masking
# missed one, and that throws rather than risking a false pass. The result
# keeps its original case and bytes: comparison must be case-sensitive.
function Get-TokenStream {
    param([string]$Text, [switch]$IgnoreSplices)

    $literals = New-Object System.Collections.ArrayList
    $masked = $Text

    # Raw strings first (they may span lines, hence Singleline), then ordinary
    # single-line strings and character literals. \x27 stands in for the single
    # quote so the pattern needs no quote escaping of its own.
    $specs = @(
        @{ Pattern = '(?:u8|u|U|L)?R"([^()\s\\]{0,16})\(.*?\)\1"'; Options = [System.Text.RegularExpressions.RegexOptions]::Singleline },
        @{ Pattern = '(?:u8|u|U|L)?"(?:[^"\\\r\n]|\\.)*"'; Options = [System.Text.RegularExpressions.RegexOptions]::None },
        @{ Pattern = '(?:u8|u|U|L)?\x27(?:[^\x27\\\r\n]|\\.)*\x27'; Options = [System.Text.RegularExpressions.RegexOptions]::None }
    )
    foreach ($spec in $specs) {
        $re = New-Object System.Text.RegularExpressions.Regex($spec.Pattern, $spec.Options)
        $builder = New-Object System.Text.StringBuilder
        $pos = 0
        foreach ($m in $re.Matches($masked)) {
            [void]$builder.Append($masked.Substring($pos, $m.Index - $pos))
            [void]$literals.Add($m.Value)
            $placeholder = ([char]1).ToString() + 'LIT' + ($literals.Count - 1) + ([char]2).ToString()
            [void]$builder.Append($placeholder)
            $pos = $m.Index + $m.Length
        }
        [void]$builder.Append($masked.Substring($pos))
        $masked = $builder.ToString()
    }

    if ($IgnoreSplices) {
        # A backslash immediately followed by a line terminator is a
        # preprocessor line splice, not a token. When clang-format joins or
        # splits the lines of a macro it adds or removes these, which changes
        # the text without changing the code.
        $masked = $masked -replace "\\\r\n|\\\n|\\\r", ''
    }

    # Block comments first so a "//" inside one cannot start a line comment.
    $masked = [regex]::Replace($masked, '/\*[\s\S]*?\*/', '')
    $masked = $masked -replace '//[^\r\n]*', ''

    # A raw-string opener surviving the masking above was not masked, so its
    # contents cannot be trusted. Fail loudly instead of mis-comparing.
    if ($masked -cmatch '(^|[^A-Za-z0-9_])(u8|u|U|L)?R"') {
        throw 'Get-TokenStream: possible C++ raw string literal that was not masked; refusing to compare.'
    }

    # Remove whitespace outright rather than folding each run to a single space.
    # Folding would preserve the mere existence of a line break, so joining
    # `0.0f` and `});` would read as a token change. Whitespace between two
    # tokens is exactly what this formatter is allowed to alter: it cannot merge
    # two identifiers, and the non-whitespace rewrites it can perform
    # (reordering using-declarations, splitting string literals, injecting
    # namespace comments, reordering includes) are all disabled in
    # .clang-format. Removing whitespace therefore cannot hide a real edit,
    # while still catching any change to identifier case or literal contents.
    $masked = [regex]::Replace($masked, '\s+', '')

    for ($i = 0; $i -lt $literals.Count; $i++) {
        $placeholder = ([char]1).ToString() + 'LIT' + $i + ([char]2).ToString()
        $masked = $masked.Replace($placeholder, [string]$literals[$i])
    }
    return $masked
}

$exitCode = 2
try {
    # Without the repository style file clang-format silently falls back to
    # LLVM defaults, which would pass or rewrite files under the wrong rules.
    if (-not (Test-Path -LiteralPath $StyleFile -PathType Leaf)) {
        Write-Error "Repository style file is missing: '$StyleFile'. Refusing to format without it." -ErrorAction Continue
        $exitCode = 2
    }
    elseif ((Get-Item -LiteralPath $StyleFile).Length -eq 0) {
        Write-Error "Repository style file is empty: '$StyleFile'. Refusing to format without it." -ErrorAction Continue
        $exitCode = 2
    }
    else {
        $StyleArg = '--style=file:' + $StyleFile

        # --- Selection -------------------------------------------------------
        # Work items are objects so each file carries its own scope: whole file
        # for untracked or explicitly named files, changed ranges otherwise.
        $work = @()

        function New-WorkItem {
            param([string]$FullPath, [bool]$WholeFile, [string[]]$Ranges)
            return [pscustomobject]@{ Path = $FullPath; WholeFile = $WholeFile; Ranges = $Ranges }
        }

        $selectionFailed = $false
        if ($All) {
            # Not $all: PowerShell variable names are case-insensitive, so $all
            # IS the [switch]$All parameter and assigning an array would coerce
            # it to a switch.
            $allSources = Get-ChildItem -Path $RepoRoot -Recurse -File |
                Where-Object { $SourceExtensions -contains $_.Extension -and $_.FullName -notmatch $ExcludedDirRe }
            foreach ($f in $allSources) { $work += New-WorkItem $f.FullName $true @() }
        }
        elseif ($Paths) {
            foreach ($p in $Paths) {
                $full = if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $RepoRoot $p }
                if (Test-Path -LiteralPath $full -PathType Container) {
                    foreach ($f in (Get-ChildItem -Path $full -Recurse -File |
                                Where-Object { $SourceExtensions -contains $_.Extension })) {
                        $work += New-WorkItem $f.FullName $true @()
                    }
                }
                elseif (Test-Path -LiteralPath $full -PathType Leaf) {
                    $work += New-WorkItem ((Resolve-Path -LiteralPath $full).Path) $true @()
                }
                else {
                    Write-Error "Path not found: '$p'. Refusing to report a clean gate for a path that did not resolve." -ErrorAction Continue
                    $selectionFailed = $true
                }
            }
        }
        else {
            $tracked = & git diff --name-only --diff-filter=ACMR $Base -- 2>$null
            if ($LASTEXITCODE -ne 0) {
                throw "git diff --name-only failed with exit code $LASTEXITCODE."
            }
            foreach ($rel in $tracked) {
                if ($SourceExtensions -notcontains [IO.Path]::GetExtension($rel)) { continue }
                $full = Join-Path $RepoRoot $rel
                if (Test-Path -LiteralPath $full -PathType Leaf) {
                    $work += New-WorkItem $full $false (Get-ChangedRanges $full $Base)
                }
            }

            # git diff cannot see untracked files, so they are formatted in full.
            $untracked = & git ls-files --others --exclude-standard -- 2>$null
            if ($LASTEXITCODE -ne 0) {
                throw "git ls-files failed with exit code $LASTEXITCODE."
            }
            foreach ($rel in $untracked) {
                if ($SourceExtensions -notcontains [IO.Path]::GetExtension($rel)) { continue }
                $full = Join-Path $RepoRoot $rel
                if (Test-Path -LiteralPath $full -PathType Leaf) {
                    $work += New-WorkItem $full $true @()
                }
            }
        }

        if ($selectionFailed) {
            $exitCode = 2
        }
        else {
            $work = @($work | Where-Object { $_.WholeFile -or $_.Ranges.Count -gt 0 } |
                    Sort-Object -Property Path -Unique)

            $formatter = Resolve-ClangFormat
            $versionOutput = & $formatter --version 2>&1
            if ($LASTEXITCODE -ne 0) {
                throw "clang-format --version failed with exit code $LASTEXITCODE."
            }
            Write-Host "clang-format: $((($versionOutput | Out-String).Trim()))"
            Write-Host "mode: $Mode   base: $(if ($All) { '<all>' } elseif ($Paths) { '<explicit paths>' } else { $Base })   files: $($work.Count)   max passes: $MaxPasses"
            Write-Host ''

            if ($work.Count -eq 0) {
                Write-Host 'No C++ sources in scope. Nothing to do.'
                $exitCode = 0
            }
            else {
                # Both modes format a scratch copy and validate before anything
                # is written back. Validating after writing would let an unsafe
                # file reach the tree with the gate already reporting failure,
                # leaving the caller no clean state to reason about.
                if (-not (Test-Path -LiteralPath $ScratchDir)) {
                    New-Item -ItemType Directory -Force -Path $ScratchDir | Out-Null
                }

                # --- Format and verify ---------------------------------------
                $drift = @()
                $unsafe = @()
                $unstable = @()
                $spliced = @()
                $fixed = @()
                $scratchIndex = 0

                foreach ($item in $work) {
                    $before = Get-Content -LiteralPath $item.Path -Raw
                    if ($null -eq $before) { continue }

                    # The copy keeps its extension so clang-format picks the
                    # same language. The style travels on the command line, so
                    # the scratch location cannot change style resolution.
                    $scratchIndex++
                    $leaf = Split-Path $item.Path -Leaf
                    $copy = Join-Path $ScratchDir ("{0:d3}-{1}" -f $scratchIndex, $leaf)
                    Copy-Item -LiteralPath $item.Path -Destination $copy -Force

                    $result = Invoke-FormatToFixedPoint -Path $copy -Ranges $item.Ranges `
                        -Formatter $formatter -StyleArg $StyleArg -Budget $MaxPasses

                    if (-not $result.Converged) {
                        $unstable += $item.Path
                        Remove-Item -LiteralPath $copy -Force -ErrorAction SilentlyContinue
                        continue
                    }

                    $formatted = Get-Content -LiteralPath $copy -Raw

                    # Token safety is checked before any write. A difference that
                    # disappears once preprocessor line splices are ignored is
                    # benign macro reflow and is allowed but reported; anything
                    # else means clang-format altered real code, so the file is
                    # reported as unsafe and left untouched. Case-sensitive
                    # comparison: case changes and literal edits are code.
                    try {
                        $strictBefore = Get-TokenStream $before
                        $strictAfter = Get-TokenStream $formatted
                    }
                    catch {
                        Write-Warning "UNSAFE  $(Get-RepoRelative $item.Path)  -- token check refused to compare: $($_.Exception.Message)"
                        $unsafe += $item.Path
                        Remove-Item -LiteralPath $copy -Force -ErrorAction SilentlyContinue
                        continue
                    }
                    if ($strictAfter -cne $strictBefore) {
                        if ((Get-TokenStream -Text $formatted -IgnoreSplices) -cne `
                            (Get-TokenStream -Text $before -IgnoreSplices)) {
                            $unsafe += $item.Path
                            Remove-Item -LiteralPath $copy -Force -ErrorAction SilentlyContinue
                            continue
                        }
                        $spliced += $item.Path
                    }

                    if ((Normalize-Text $formatted) -cne (Normalize-Text $before)) {
                        if ($Mode -eq 'Check') {
                            $drift += $item.Path
                        }
                        else {
                            Copy-Item -LiteralPath $copy -Destination $item.Path -Force
                            $fixed += $item.Path
                        }
                    }

                    Remove-Item -LiteralPath $copy -Force -ErrorAction SilentlyContinue
                }

                # --- Report --------------------------------------------------
                $fixCmd = 'pwsh -NoProfile -File scripts/Format-Cpp.ps1 -Mode Fix'

                foreach ($p in $spliced) {
                    Write-Host "SPLICE  $(Get-RepoRelative $p)  -- macro line splices reflowed; code unchanged" -ForegroundColor DarkGray
                }
                foreach ($p in $unstable) {
                    Write-Host "UNSTABLE $(Get-RepoRelative $p)  -- no fixed point within $MaxPasses passes" -ForegroundColor Red
                }
                foreach ($p in $unsafe) {
                    Write-Host "UNSAFE  $(Get-RepoRelative $p)  -- formatter altered the code token stream" -ForegroundColor Red
                }

                if ($Mode -eq 'Check') {
                    foreach ($p in $drift) {
                        Write-Host "DRIFT   $(Get-RepoRelative $p)" -ForegroundColor Yellow
                    }

                    Write-Host ''
                    $failed = $drift.Count + $unsafe.Count + $unstable.Count
                    if ($failed -gt 0) {
                        Write-Host "Check FAILED: $($drift.Count) drifted, $($unsafe.Count) token-unsafe, $($unstable.Count) non-convergent."
                        Write-Host "Fix with: $fixCmd"
                        $exitCode = 1
                    }
                    else {
                        Write-Host "Check PASSED: $($work.Count) file(s) in scope are canonically formatted."
                        $exitCode = 0
                    }
                }
                else {
                    foreach ($p in $fixed) {
                        Write-Host "FIXED   $(Get-RepoRelative $p)" -ForegroundColor Cyan
                    }

                    Write-Host ''
                    if ($unstable.Count -gt 0 -or $unsafe.Count -gt 0) {
                        Write-Host "Fix INCOMPLETE: $($fixed.Count) rewritten, $($unstable.Count) non-convergent, $($unsafe.Count) token-unsafe."
                        $exitCode = 1
                    }
                    else {
                        Write-Host "Fix COMPLETE: $($fixed.Count) of $($work.Count) file(s) rewritten."
                        $exitCode = 0
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
    # Only this invocation's directory is removed, so concurrent runs cannot
    # delete each other's files. Cleanup runs on success and failure alike.
    if (Test-Path -LiteralPath $ScratchDir) {
        Remove-Item -LiteralPath $ScratchDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    Pop-Location
}
exit $exitCode
