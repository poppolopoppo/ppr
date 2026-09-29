<#
.SYNOPSIS
    PPR semantic-control-flow tripwire. Scans C++ sources for four defect
    classes and exits 1 when hits exist (CI-gating semantics), 0 when clean.

.DESCRIPTION
    Checks map to AGENTS.md "C++ and API rules / Semantic control flow":
      "Multi-line conditions use trailing operators with one operand per
        line: the operator ends its line and the next operand starts the
        next line. Never start a continuation line with `and`/`or`,
        never leave an operator stranded alone on its line, and never
        write `and(`/`or(` without a trailing space."
      "Separate distinct logical phases (setup, validation, iteration,
        submission, logging) with blank lines; do not put a blank line
        between every statement."
      "Mechanical reformat_file ... never inserts blank lines, never rebreaks
        and/or/not chains, and never spaces alternative tokens (and(, or(, x<).
        The semantic pass owns all three."

    Class map:
      A-STRANDED  -> "never leave an operator stranded alone on its line";
                     trailing `not` (operand must stay attached: `and not x`).
      B-OP        -> hard gate: never `and(`/`or(`/`not(` without trailing
                     space after the alternative token.
      B-CMP       -> advisory: `x<` needs eyeballing — comparison `x<max`
                     is a hit, template `view<T>` is not; the script cannot
                     distinguish, so B-CMP is review-only.
      C-LEADING   -> trailing-operator rule (operator must end the line,
                     never start the continuation: a code line whose first
                     token is `and`/`or`).
      C-DETACHED  -> trailing-continuation subset where the block `{` opens
                     lone on a following line instead of attached to the
                     `)` line.
      D-DENSE     -> "separate distinct logical phases with blank lines".

.PARAMETER PathFilter
    Optional case-insensitive substring; only files whose full path contains
    it are scanned. E.g. -PathFilter 'game\colony'.

.PARAMETER RepoRoot
    Defaults to the grandparent of this script (i.e. repo root when the script
    lives in scripts/), else the current directory.

.PARAMETER DenseMinCodeLines
    Class-D threshold: functions with at least this many code lines and zero
    blank lines are flagged. Default 30. Tuned so the ~80-line zero-blank
    agentIntent body (game/colony/Agents.cpp) fires without flagging small
    helpers across the engine.

.EXAMPLE
    pwsh -NoProfile -File scripts/Check-SemanticStyle.ps1
    pwsh -NoProfile -File scripts/Check-SemanticStyle.ps1 -PathFilter 'game\colony'

.NOTES
    Stock pwsh 5.1 compatible. No modules. Read-only (never modifies files).
    `exit` sets the process exit code for gating; do not dot-source this file.
#>
param(
    [string]$PathFilter = "",
    [string]$RepoRoot = "",
    [int]$DenseMinCodeLines = 30
)

$ErrorActionPreference = 'Stop'

# --- Resolve repo root -------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    if ($PSScriptRoot) {
        $RepoRoot = Split-Path $PSScriptRoot -Parent   # scripts/ -> repo root
    } else {
        $RepoRoot = (Get-Location).Path
    }
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

# --- Scope -------------------------------------------------------------------
# Verified repo layout (2026-09-29): root holds game/, lib/, include/,
# assets/, cmake/, docs/, local-services/, plus build/output dirs out/,
# Temp/, Testing/, _deps (CMake FetchContent), vcpkg_installed, and tool
# state .slim/, .idea/, .vscode/, .git/, .codegraph/, .windbg-symbol-cache/.
# Scan only real sources; everything below is explicitly excluded.
$ExcludeDirRe = '(^|[\\/])(out|build|cmake-build[^\\/]*|Temp|Testing|_deps|vcpkg_installed|\.slim|\.idea|\.vscode|\.git|\.codegraph|\.windbg-symbol-cache|local-services[\\/]pix|captures|_staging)([\\/]|$)'
$ExcludeFileRe = '(\.generated|\.pb)\.(cpp|cppm|h)$'
$SourceExts = @('.cpp', '.cppm', '.h')

$files = Get-ChildItem -Path $RepoRoot -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object {
        $SourceExts -contains $_.Extension -and
        ($_.FullName -notmatch $ExcludeDirRe) -and
        ($_.Name -notmatch $ExcludeFileRe)
    }
if (-not [string]::IsNullOrEmpty($PathFilter)) {
    $files = $files | Where-Object {
        $_.FullName.IndexOf($PathFilter, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
    }
}

# --- Line model --------------------------------------------------------------
# Per line we keep: original text, code-only text (strings/comments
# stripped), and blank/comment-only flags. Naive stripping: block comments
# tracked across lines; // tails cut; "..." / '...' literals blanked first
# so comment markers inside strings survive. BLIND SPOTS: raw strings
# (R"(...)"), line continuations inside strings, and // inside char
# literals can mis-strip; hits on such lines need eyeballing.
function Split-Lines([string]$FilePath) {
    $raw = Get-Content -LiteralPath $FilePath
    if ($null -eq $raw) { return @() }
    if ($raw -is [string]) { $raw = @($raw) }
    $out = @()
    $inBlock = $false
    foreach ($text in $raw) {
        $code = $text
        $commentOnly = $false
        if ($inBlock) {
            $end = $code.IndexOf('*/')
            if ($end -ge 0) {
                $code = $code.Substring($end + 2)
                $inBlock = $false
            } else {
                $code = ''
                if ($text.Trim() -eq '') { $commentOnly = $false }
                else { $commentOnly = $true }
                $out += [pscustomobject]@{ Text = $text; Code = ''; Blank = ($text.Trim() -eq ''); CommentOnly = $commentOnly; Preproc = $false; Indent = 0 }
                continue
            }
        }
        # Blank string/char literals so their contents cannot match.
        $code = [regex]::Replace($code, '"(?:\\.|[^"\\])*"', '""')
        $code = [regex]::Replace($code, "'(?:\\.|[^'\\])*'", "''")
        # Inline /* ... */ pairs; an unterminated /* opens block mode.
        while ($true) {
            $s = $code.IndexOf('/*')
            if ($s -lt 0) { break }
            $e = $code.IndexOf('*/', $s + 2)
            if ($e -ge 0) { $code = $code.Substring(0, $s) + ' ' + $code.Substring($e + 2) }
            else { $code = $code.Substring(0, $s); $inBlock = $true; break }
        }
        $c = $code.IndexOf('//')
        if ($c -ge 0) { $code = $code.Substring(0, $c) }
        $trim = $text.Trim()
        if ($trim -eq '') {
            $out += [pscustomobject]@{ Text = $text; Code = ''; Blank = $true; CommentOnly = $false; Preproc = $false; Indent = 0 }
        } else {
            $isComment = ($code.Trim() -eq '') -and ($trim.StartsWith('//') -or $trim.StartsWith('/*') -or $inBlock -or $trim.StartsWith('*'))
            $m = [regex]::Match($text, '^(\s*)')
            $out += [pscustomobject]@{
                Text        = $text
                Code        = $code
                Blank       = $false
                CommentOnly = $isComment
                Preproc     = ($code -cmatch '^\s*#')
                Indent      = $m.Groups[1].Value.Length
            }
        }
    }
    return $out
}

function Get-RelPath([string]$Full) {
    $rel = $Full
    if ($Full.StartsWith($RepoRoot)) {
        $rel = $Full.Substring($RepoRoot.Length).TrimStart('\', '/')
    }
    return $rel
}

function Get-Excerpt([string]$Text, [int]$Max = 140) {
    $e = $Text.Trim()
    if ($e.Length -gt $Max) { $e = $e.Substring(0, $Max) + '...' }
    return $e
}

$hits = @()   # each: Rel, Line, Class, Excerpt
$scanned = 0

foreach ($f in $files) {
    $scanned++
    $rel = Get-RelPath $f.FullName
    $ext = $f.Extension
    $L = Split-Lines $f.FullName
    $n = $L.Count

    # --- Classes A / B / C: line-local (+ small lookahead for C) -------------
    for ($i = 0; $i -lt $n; $i++) {
        $li = $L[$i]
        if ($li.Blank -or $li.CommentOnly -or $li.Preproc) { continue }
        $code = $li.Code
        if ([string]::IsNullOrWhiteSpace($code)) { continue }
        $lineNo = $i + 1

        # A-STRANDED: `and`/`or` alone on its line (P6 tripwire core).
        # \b guards avoid `for`/`error`/`error_code` (word chars join).
        if ($code -cmatch '^\s*(and|or)\s*$') {
            $hits += [pscustomobject]@{ Rel = $rel; Line = $lineNo; Class = 'A-STRANDED'; Excerpt = Get-Excerpt $li.Text }
        }
        # A-STRANDED: line ends with `not`; operand sits on the next line.
        elseif ($code -cmatch '\bnot\s*$') {
            $hits += [pscustomobject]@{ Rel = $rel; Line = $lineNo; Class = 'A-STRANDED'; Excerpt = Get-Excerpt $li.Text }
        }

        # B-OP: and(/or(/not( — missing space after alternative token.
        if ($code -cmatch '\b(and|or|not)\(') {
            $hits += [pscustomobject]@{ Rel = $rel; Line = $lineNo; Class = 'B-OP'; Excerpt = Get-Excerpt $li.Text }
        }

        # B-CMP (advisory, review-only): identifier/keyword directly before
        # `<` (the `x<` case), gated to condition position for precision:
        # line must mention a condition keyword or an alternative token.
        # Skips preprocessor (#include <...>), `template<...>` headers,
        # operator< overloads, and <<, <=, <=>, ->, ::< via the char classes
        # / lookahead. Cannot distinguish comparison `x<max` (hit) from
        # template `view<T>` (not a hit), so eyeball every B-CMP hit.
        $condPos = $code -match '\b(if|else|for|while|return|and|or|not|assert|EXPECT|CHECK|static_assert|requires)\b'
        if ($condPos -and ($code -cmatch '^\s*template\b') -eq $false -and ($code -cmatch '\boperator\s*<') -eq $false) {
            if ($code -cmatch '[\w\)\]"'']<(?![<=>])') {
                $hits += [pscustomobject]@{ Rel = $rel; Line = $lineNo; Class = 'B-CMP'; Excerpt = Get-Excerpt $li.Text }
            }
        }

        # C-LEADING: trailing-operator rule — a code line whose first token
        # is `and`/`or` starts a continuation with the operator (canonical
        # = operator ends the previous line). Guard: a line that is ONLY
        # the operator already fired A-STRANDED above; do not double-hit.
        if (($code -cmatch '^\s*(and|or)\s*$') -eq $false -and ($code -cmatch '^\s*(and|or)\b')) {
            $hits += [pscustomobject]@{ Rel = $rel; Line = $lineNo; Class = 'C-LEADING'; Excerpt = Get-Excerpt $li.Text }
        }

        # C-DETACHED: triggered from a canonical trailing-`and`/`or` line
        # (no hit itself); escalate only when the block `{` opens lone on
        # a following line. Allman-style function definitions never have
        # a trailing-and/or line before `{`, so precision is preserved.
        # Look ahead over blanks/comment-only lines; stop with no hit when
        # the `{` is attached to the `)` line or statements intervene.
        if ($code -cmatch '\b(and|or)\s*$') {
            # C-DETACHED: the block `{` opens lone on a following line
            # (canonical = attached to the `)` line) instead. Look ahead
            # over blanks/comment-only lines; escalate only on a lone `{`
            # with a `)` seen in between (ties the brace to the condition).
            $seenClose = ($code -cmatch '\)')
            for ($j = $i + 1; $j -lt $n -and $j -le ($i + 8); $j++) {
                $lj = $L[$j]
                if ($lj.Blank -or $lj.CommentOnly) { continue }
                if ($lj.Preproc) { break }
                if ($lj.Code -cmatch '\)') { $seenClose = $true }
                if ($lj.Code -cmatch '^\s*\{\s*$') {
                    if ($seenClose) {
                        $hits += [pscustomobject]@{ Rel = $rel; Line = ($j + 1); Class = 'C-DETACHED'; Excerpt = Get-Excerpt $lj.Text }
                    }
                    break
                }
                if ($lj.Code -cmatch '\{\s*$') { break }  # attached -> canonical, stop
                if ($lj.Code -cmatch ';\s*$') { break }   # ran into statements, stop
            }
        }
    }

    # --- Class D: dense functions (.cpp only) --------------------------------
    # Heuristic: brace-matched function bodies with >= DenseMinCodeLines code
    # lines and ZERO blank lines. Comment-only lines are separators: they
    # neither count as code nor satisfy the blank-line requirement.
    # Fires on agentIntent-style density (fetch/validate/step/path/ladder/
    # decay phases with no visual breaks). BLIND SPOTS (precision-first):
    # a single blank line anywhere clears the function; lambdas/macros and
    # EXPORT-macro prefixes can defeat signature matching; naive brace
    # counting mis-tracks braces inside un-stripped constructs; .h/.cppm
    # skipped (dense declarations are normal there); .cpp-only by design.
    if ($ext -eq '.cpp') {
        $depth = 0
        $i = 0
        while ($i -lt $n) {
            $li = $L[$i]
            $candidate = $false
            if (-not $li.Blank -and -not $li.CommentOnly -and -not $li.Preproc) {
                $c = $li.Code
                $paren = $c.IndexOf('(')
                if ($paren -ge 0 -and ($c -cmatch '\)') -and ($c.TrimEnd() -notmatch ';\s*$')) {
                    $pre = $c.Substring(0, $paren)
                    if (($pre -cmatch '[A-Za-z_][\w:]*\s*$') -and
                        ($c -cmatch '^\s*(if|for|while|switch|catch|else)\b') -eq $false -and
                        ($pre -cmatch '=') -eq $false -and
                        ($c -cmatch '^\s*(class|struct|enum|namespace)\b') -eq $false -and
                        $depth -le 2) {
                        if ($c -cmatch '\{\s*$') { $candidate = $true }
                        else {
                            for ($k = $i + 1; $k -lt $n -and $k -le ($i + 3); $k++) {
                                if ($L[$k].Blank -or $L[$k].CommentOnly) { continue }
                                if ($L[$k].Code -cmatch '^\s*\{\s*$') { $candidate = $true }
                                break
                            }
                        }
                    }
                }
            }
            if (-not $candidate) {
                $depth += ([regex]::Matches($li.Code, '\{')).Count - ([regex]::Matches($li.Code, '\}')).Count
                $i++
                continue
            }
            # Walk the body with brace matching from the opening line.
            $sigLine = $i + 1
            $sigText = $li.Text
            $d = $depth
            $openIdx = -1
            $j = $i
            while ($j -lt $n) {
                $d += ([regex]::Matches($L[$j].Code, '\{')).Count - ([regex]::Matches($L[$j].Code, '\}')).Count
                if ($openIdx -eq -1 -and ($L[$j].Code -cmatch '\{')) { $openIdx = $j }
                if ($openIdx -ge 0 -and $d -le $depth) { break }
                $j++
                if (($j - $i) -gt 3000) { break }  # runaway guard
            }
            if ($openIdx -ge 0 -and $j -lt $n) {
                $codeCount = 0; $blankCount = 0
                for ($k = $openIdx; $k -le $j; $k++) {
                    if ($L[$k].Blank) { $blankCount++ }
                    elseif (-not $L[$k].CommentOnly -and -not [string]::IsNullOrWhiteSpace($L[$k].Code)) { $codeCount++ }
                }
                if ($codeCount -ge $DenseMinCodeLines -and $blankCount -eq 0) {
                    $hits += [pscustomobject]@{
                        Rel     = $rel
                        Line    = $sigLine
                        Class   = 'D-DENSE'
                        Excerpt = (Get-Excerpt $sigText 100) + "  [$codeCount code lines, 0 blanks]"
                    }
                }
                $depth = $d
                $i = $j + 1
            } else {
                $depth += ([regex]::Matches($li.Code, '\{')).Count - ([regex]::Matches($li.Code, '\}')).Count
                $i++
            }
        }
    }
}

# --- Output ------------------------------------------------------------------
foreach ($h in ($hits | Sort-Object Rel, Line, Class)) {
    Write-Output ('{0}:{1}:{2}: {3}' -f $h.Rel, $h.Line, $h.Class, $h.Excerpt)
}

Write-Output ''
Write-Output ("Files scanned: {0}   Hits: {1}" -f $scanned, $hits.Count)
if ($hits.Count -gt 0) {
    Write-Output '-- by class --'
    $hits | Group-Object Class | Sort-Object Name |
        ForEach-Object { Write-Output ("  {0}: {1}" -f $_.Name, $_.Count) }
    Write-Output '-- by directory --'
    $hits | ForEach-Object {
        $d = Split-Path $_.Rel -Parent
        if ([string]::IsNullOrEmpty($d)) { $d = '.' }
        $d
    } | Group-Object | Sort-Object Count -Descending |
        ForEach-Object { Write-Output ("  {0}: {1}" -f $_.Name, $_.Count) }
    exit 1
} else {
    Write-Output 'Clean.'
    exit 0
}
