#Requires -Version 7
[CmdletBinding()]
param(
    [string[]]$Pathspec,
    [switch]$AllLocal,
    [ValidateRange(1, 15)][int]$MaxFiles = 15,
    [ValidateRange(1, 204800)][int]$MaxBytes = 204800
)
$ErrorActionPreference = 'Stop'

function Invoke-GitBytes([string[]]$Arguments) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = 'git'
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$startInfo.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    [void]$process.Start()
    $stream = [System.IO.MemoryStream]::new()
    $process.StandardOutput.BaseStream.CopyTo($stream)
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "git $($Arguments -join ' ') failed: $stderr" }
    return ,$stream.ToArray()
}

function ConvertFrom-NameStatus([byte[]]$Bytes) {
    $tokens = [System.Text.Encoding]::UTF8.GetString($Bytes).Split([char]0, [System.StringSplitOptions]::None)
    $units = @()
    for ($index = 0; $index -lt $tokens.Count - 1;) {
        $status = $tokens[$index++]
        if ([string]::IsNullOrEmpty($status)) { continue }
        if ($status -match '^[RC]') {
            if ($index + 1 -ge $tokens.Count) { throw 'malformed NUL-delimited rename/copy status output' }
            $paths = @($tokens[$index++], $tokens[$index++])
        } else {
            if ($index -ge $tokens.Count) { throw 'malformed NUL-delimited status output' }
            $paths = @($tokens[$index++])
        }
        if (@($paths | Where-Object { [string]::IsNullOrEmpty($_) }).Count) { throw 'empty path in git status output' }
        $units += [pscustomobject]@{ status = $status; paths = $paths }
    }
    $units
}

function Test-SelectedPath([string]$Path, [string[]]$Selections) {
    if (-not $Selections.Count) { return $true }
    foreach ($selection in $Selections) {
        $trimmed = $selection.TrimEnd([char[]]@('/', [char]92))
        if ($Path -eq $trimmed -or $Path.StartsWith("$trimmed/", [System.StringComparison]::Ordinal)) { return $true }
    }
    $false
}

function Get-PatchByteCount([string]$Mode, [string[]]$Paths) {
    $arguments = if ($Mode -eq 'all-local') { @('diff', '--binary', 'HEAD', '--') } else { @('diff', '--cached', '--binary', 'HEAD', '--') }
    $arguments += @($Paths | ForEach-Object { ":(literal)$_" })
    (Invoke-GitBytes $arguments).Length
}

$status = (& git status --porcelain=v1 --branch) -join "`n"
if ($status -match '(?m)^(DD|AU|UD|UA|DU|AA|UU) ') { throw 'merge conflicts present' }
if ($status -match '(?m)^## .*no branch') { throw 'detached HEAD' }

$mode = if ($AllLocal) { 'all-local' } else { 'staged' }
$diffArguments = if ($AllLocal) { @('diff', '--name-status', '--find-renames', '-z', 'HEAD', '--') } else { @('diff', '--cached', '--name-status', '--find-renames', '-z', 'HEAD', '--') }
$units = @(ConvertFrom-NameStatus (Invoke-GitBytes $diffArguments))
$selectedUnits = @($units | Where-Object { @($_.paths | Where-Object { Test-SelectedPath $_ $Pathspec }).Count })

$untracked = [System.Text.Encoding]::UTF8.GetString((Invoke-GitBytes @('ls-files', '--others', '--exclude-standard', '-z'))).Split([char]0, [System.StringSplitOptions]::RemoveEmptyEntries)
$matchingUntracked = @($untracked | Where-Object { Test-SelectedPath $_ $Pathspec })
if ($AllLocal -and $matchingUntracked.Count) { throw "untracked files match the literal selection and are unsupported: $($matchingUntracked -join ', ')" }
if (-not $selectedUnits.Count) { throw 'no applicable tracked changes' }

$candidates = @()
$groups = $selectedUnits | Group-Object { $parent = Split-Path $_.paths[-1] -Parent; if ($parent) { $parent.Replace([string][char]92, '/') } else { '.' } } | Sort-Object Name
foreach ($group in $groups) {
    $chunk = @(); $fileCount = 0; $estimatedPatchBytes = 0
    foreach ($unit in @($group.Group | Sort-Object { $_.paths[-1] })) {
        $unitBytes = Get-PatchByteCount $mode $unit.paths
        $unitFileCount = @($unit.paths).Count
        if ($unitFileCount -gt $MaxFiles -or $unitBytes -gt $MaxBytes) { throw "single change unit exceeds artifact gate: $($unit.paths -join ' -> ')" }
        if ($chunk.Count -and ($fileCount + $unitFileCount -gt $MaxFiles -or $estimatedPatchBytes + $unitBytes -gt $MaxBytes)) {
            $candidates += [ordered]@{ scope = $group.Name; files = @($chunk.paths | ForEach-Object { $_ }); file_count = $fileCount; estimated_patch_bytes = $estimatedPatchBytes; units = @($chunk) }
            $chunk = @(); $fileCount = 0; $estimatedPatchBytes = 0
        }
        $chunk += [ordered]@{ status = $unit.status; paths = @($unit.paths); estimated_patch_bytes = $unitBytes }
        $fileCount += $unitFileCount; $estimatedPatchBytes += $unitBytes
    }
    if ($chunk.Count) { $candidates += [ordered]@{ scope = $group.Name; files = @($chunk.paths | ForEach-Object { $_ }); file_count = $fileCount; estimated_patch_bytes = $estimatedPatchBytes; units = @($chunk) } }
}

$next = @($candidates | Select-Object -First 8)
$totalBatchCount = $candidates.Count
foreach ($candidate in $next) { $candidate.total_batch_count = $totalBatchCount; $candidate.remaining_batch_count = $totalBatchCount - 1 }
[ordered]@{
    schema_version = 1
    mode = $mode
    selection = @($Pathspec)
    candidates = $next
    total_batch_count = $totalBatchCount
    remaining_batch_count = $totalBatchCount - $next.Count
    grouping_schema = 'Selected groupings carry discovery_selection.total_batch_count from their candidate; the builder reports total_batch_count minus one.'
} | ConvertTo-Json -Depth 8
