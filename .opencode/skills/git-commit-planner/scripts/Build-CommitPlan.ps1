#Requires -Version 7
[CmdletBinding(DefaultParameterSetName = 'Build')]
param(
    [Parameter(ParameterSetName = 'Build', Mandatory = $true)][string]$GroupingJson,
    [Parameter(ParameterSetName = 'Verify', Mandatory = $true)][switch]$VerifyOnly,
    [Parameter(ParameterSetName = 'Verify', Mandatory = $true)][string]$PlanJson,
    [string]$Out = '.slim/commit-plan.json'
)
$ErrorActionPreference = 'Stop'
$artifactLimitBytes = 1048576

function Get-Sha256Bytes([byte[]]$Bytes) { ([BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($Bytes))).Replace('-', '').ToLowerInvariant() }
function Get-Sha256([string]$Text) { Get-Sha256Bytes ([Text.Encoding]::UTF8.GetBytes($Text)) }
function Invoke-GitBytes([string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new('git')
    $start.UseShellExecute = $false; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $start; [void]$process.Start()
    $stderrTask = $process.StandardError.ReadToEndAsync(); $output = [IO.MemoryStream]::new(); $process.StandardOutput.BaseStream.CopyTo($output); $process.WaitForExit(); $error = $stderrTask.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "git $($Arguments -join ' ') failed: $error" }
    $output.ToArray()
}
function Invoke-Git([string[]]$Arguments) { [Text.Encoding]::UTF8.GetString((Invoke-GitBytes $Arguments)) }
function New-OrdinalPathSet { Write-Output -NoEnumerate ([Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)) }
function Get-OrdinalUniquePaths([string[]]$Paths) { $seen = New-OrdinalPathSet; $unique = [Collections.Generic.List[string]]::new(); foreach ($path in $Paths) { if ($seen.Add($path)) { $unique.Add($path) } }; $unique.ToArray() }
function Wrap-Text([string]$Text) { $out = @(); $line = ''; foreach ($word in ($Text -split '\s+')) { if ($line.Length -eq 0) { $line = $word } elseif ($line.Length + $word.Length + 1 -le 72) { $line += " $word" } else { $out += $line; $line = $word } }; if ($line) { $out += $line }; $out -join "`n" }
function Test-Message($Step, [int]$Index) { $e = @(); $subject = "$($Step.component): $($Step.subject)"; if ([string]::IsNullOrWhiteSpace($Step.component) -or [string]::IsNullOrWhiteSpace($Step.subject)) { $e += "R$Index`: empty message component or subject" }; if ($subject.Length -gt 72) { $e += "R$Index`: subject exceeds 72 characters" }; if ($Step.subject -match '^(feat|fix|chore|refactor|docs|test|style)(\(.+\))?:' -or $Step.subject -match '\.' -or $Step.subject -cmatch '^[A-Z]') { $e += "R$Index`: invalid subject convention" }; if ([string]::IsNullOrWhiteSpace($Step.body) -or @($Step.body -split "`n" | Where-Object { $_.Length -gt 72 }).Count) { $e += "R$Index`: invalid body" }; if ($Step.message -ne "$($Step.component): $($Step.subject)`n`n$($Step.body)") { $e += "R$Index`: message mismatch" }; $e }
function Get-PreExecutionSnapshot { [ordered]@{ head = (Invoke-Git @('rev-parse', 'HEAD')).Trim(); branch = (Invoke-Git @('symbolic-ref', '--quiet', '--short', 'HEAD')).Trim(); index_fingerprint = Get-Sha256Bytes (Invoke-GitBytes @('ls-files', '-s', '-z')) } }
function Get-Patch([string]$Mode, [string[]]$Paths) { $literalPaths = @($Paths | ForEach-Object { ":(literal)$_" }); if ($Mode -eq 'staged') { $patch = Invoke-GitBytes (@('diff', '--cached', '--binary', 'HEAD', '--') + $literalPaths) } elseif ($Mode -eq 'all-local') { $patch = Invoke-GitBytes (@('diff', '--binary', 'HEAD', '--') + $literalPaths) } else { throw "unsupported content mode: $Mode" }; if ($patch.Length -eq 0) { throw "selected $Mode content is empty" }; [Convert]::ToBase64String($patch) }
function Get-TouchedPaths([string]$Mode, [string[]]$Paths) { $literalPaths = @($Paths | ForEach-Object { ":(literal)$_" }); $diffArgs = if ($Mode -eq 'staged') { @('diff', '--cached', '--name-status', '--find-renames', 'HEAD', '--') } else { @('diff', '--name-status', '--find-renames', 'HEAD', '--') }; $lines = (Invoke-Git (@('-c', 'core.quotePath=false') + $diffArgs + $literalPaths)) -split "`n"; $touched = @($Paths); foreach ($line in $lines) { $parts = $line -split "`t"; if ($parts[0] -match '^[RC]' -and $parts.Count -ge 3) { $touched += $parts[1], $parts[2] } elseif ($parts.Count -ge 2) { $touched += $parts[1] } }; Get-OrdinalUniquePaths $touched }
function Test-PayloadPaths($Plan, $Step, [int]$Index) {
    $tempIndex = Join-Path ([IO.Path]::GetTempPath()) ("commit-plan-verify-$PID-$([Guid]::NewGuid().ToString('N')).index")
    $patchFile = "$tempIndex.patch"; $oldIndex = $env:GIT_INDEX_FILE
    try {
        $env:GIT_INDEX_FILE = $tempIndex; [void](Invoke-GitBytes @('read-tree', $Plan.pre_execution_snapshot.head))
        [IO.File]::WriteAllBytes($patchFile, [Convert]::FromBase64String($Step.patch_b64))
        [void](Invoke-GitBytes @('apply', '--cached', '--binary', '--whitespace=nowarn', $patchFile))
        $actual = [Text.Encoding]::UTF8.GetString((Invoke-GitBytes @('diff', '--cached', '--name-only', '-z', $Plan.pre_execution_snapshot.head))).Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)
        $approved = New-OrdinalPathSet; foreach ($path in @($Step.touched_paths)) { [void]$approved.Add($path) }; $unexpected = @($actual | Where-Object { -not $approved.Contains($_) })
        if (-not $actual.Count) { return "R$Index`: payload has no touched paths" }
        if ($unexpected.Count) { return "R$Index`: payload touches unapproved path(s): $($unexpected -join ', ')" }
    } catch { return "R$Index`: payload cannot be safely replayed: $($_.Exception.Message)" } finally {
        $env:GIT_INDEX_FILE = $oldIndex; Remove-Item -LiteralPath $patchFile -Force -ErrorAction SilentlyContinue; Remove-Item -LiteralPath $tempIndex -Force -ErrorAction SilentlyContinue
    }
}
function Test-Plan($Plan) {
    $e = @(); if ($Plan.schema_version -ne 4) { $e += "unsupported schema_version $($Plan.schema_version): re-plan with Build-CommitPlan.ps1"; return $e }
    $snapshot = $Plan.pre_execution_snapshot
    if ($null -eq $snapshot -or [string]::IsNullOrWhiteSpace($snapshot.head) -or [string]::IsNullOrWhiteSpace($snapshot.branch) -or $snapshot.index_fingerprint -notmatch '^[0-9a-f]{64}$') { $e += 'missing complete pre-execution HEAD, branch, or index fingerprint' }
    $steps = @($Plan.steps); if (-not $steps.Count) { $e += 'no steps' }; $seenApproved = New-OrdinalPathSet; $seenTouched = New-OrdinalPathSet
    for ($i = 0; $i -lt $steps.Count; $i++) { $step = $steps[$i]; $n = $i + 1; $stepErrors = @(); $approved = New-OrdinalPathSet; if ($step.index -ne $n -or @($step.add_paths).Count -eq 0 -or @($step.touched_paths).Count -eq 0 -or [string]::IsNullOrWhiteSpace($step.patch_b64)) { $stepErrors += "R$n`: invalid ordered replay payload" }; if ("$($step.content_mode)" -notin @('staged', 'all-local')) { $stepErrors += "R$n`: unsupported content mode $($step.content_mode)" }; $stepErrors += Test-Message $step $n; foreach ($path in @($step.add_paths)) { if ([string]::IsNullOrWhiteSpace($path) -or $path.Contains([char]0)) { $stepErrors += "R$n`: invalid approved path" }; if (-not $approved.Add($path)) { $stepErrors += "R$n`: duplicate approved path $path" }; if (-not $seenApproved.Add($path)) { $stepErrors += "R$n`: duplicate approved path $path" } }; foreach ($path in @($step.touched_paths)) { if (-not $approved.Contains($path)) { $stepErrors += "R$n`: touched path is outside approved paths: $path" }; if (-not $seenTouched.Add($path)) { $stepErrors += "R$n`: duplicate touched path $path" } }; try { $bytes = [Convert]::FromBase64String($step.patch_b64); if ((Get-Sha256Bytes $bytes) -ne $step.payload_sha256) { $stepErrors += "R$n`: payload hash mismatch" } } catch { $stepErrors += "R$n`: invalid patch payload" }; if (-not $stepErrors.Count) { $payloadError = Test-PayloadPaths $Plan $step $n; if ($payloadError) { $stepErrors += $payloadError } }; $e += $stepErrors }
    $e
}
if ($VerifyOnly) { $planBytes = [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($PlanJson)); $errors = @(Test-Plan ([Text.Encoding]::UTF8.GetString($planBytes) | ConvertFrom-Json -Depth 16)); if ($errors.Count) { $errors | ForEach-Object { Write-Error $_ }; exit 1 }; Write-Output "gate: schema-v4 plan ID=$(Get-Sha256Bytes $planBytes) and approved payload paths OK"; exit 0 }

$grouping = Get-Content $GroupingJson -Raw | ConvertFrom-Json -Depth 12; $normalizedGrouping = [IO.Path]::GetFullPath($GroupingJson); $normalizedOut = [IO.Path]::GetFullPath($Out); $defaultOut = [IO.Path]::GetFullPath((Join-Path (Get-Location) '.slim/commit-plan.json')); $defaultGrouping = [IO.Path]::GetFullPath((Join-Path (Get-Location) '.slim/grouping.json'))
if ($normalizedGrouping -eq $normalizedOut) { throw 'GroupingJson and Out must not identify the same file' }; if (Invoke-Git @('ls-files', '-u')) { throw 'merge conflicts present — clean the tree first' }
$snapshot = Get-PreExecutionSnapshot; if (-not $snapshot.head) { throw 'HEAD is required' }; if ((Invoke-Git @('status', '--porcelain=v1', '--branch')) -match '(?m)^## .*no branch') { throw 'detached HEAD — clean the tree first' }
$remainingBatchCount = 'unknown'; if ($null -ne $grouping.discovery_selection) { $value = $grouping.discovery_selection.total_batch_count; if ($value -isnot [long] -or $value -lt 1) { throw 'invalid discovery_selection.total_batch_count' }; $remainingBatchCount = $value - 1 }
$steps = @(); $used = New-OrdinalPathSet; $index = 0
foreach ($group in @($grouping.commits)) { $index++; $paths = @($group.files); if (-not $paths.Count) { throw "empty commit group $index" }; $mode = if ($group.content_mode) { "$($group.content_mode)" } else { 'staged' }; foreach ($path in $paths) { if (-not $used.Add($path)) { throw "duplicate path across steps: $path" } }; $body = Wrap-Text "$($group.body)"; $payload = Get-Patch $mode $paths; $steps += [ordered]@{ index = $index; component = "$($group.component)"; subject = "$($group.subject)"; body = $body; message = "$($group.component): $($group.subject)`n`n$body"; add_paths = $paths; touched_paths = Get-TouchedPaths $mode $paths; content_mode = $mode; patch_b64 = $payload; payload_sha256 = Get-Sha256Bytes ([Convert]::FromBase64String($payload)); notes = "$($group.notes)" } }
$plan = [ordered]@{ schema_version = 4; pre_execution_snapshot = $snapshot; steps = $steps; summary = [ordered]@{ total_steps = $steps.Count; selected_scope = @($steps.add_paths); selected_mode = @($steps.content_mode | Select-Object -Unique); remaining_batch_count = $remainingBatchCount; remainder_guidance = $(if ($remainingBatchCount -eq 'unknown') { 'Unknown: run batch discovery again before planning another scope.' } else { 'Run batch discovery again before planning another scope.' }); replay = 'isolated temporary index' } }
$serialized = $plan | ConvertTo-Json -Depth 16
if ([Text.Encoding]::UTF8.GetByteCount($serialized) -gt $artifactLimitBytes) { throw "serialized plan exceeds $artifactLimitBytes byte artifact gate" }
$tmp = "$Out.$PID.tmp"; $dir = Split-Path $Out -Parent; if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }; [IO.File]::WriteAllText([IO.Path]::GetFullPath($tmp), $serialized, [Text.UTF8Encoding]::new($false))
$errors = @(Test-Plan ([IO.File]::ReadAllText([IO.Path]::GetFullPath($tmp), [Text.Encoding]::UTF8) | ConvertFrom-Json -Depth 16)); if ($errors.Count) { $errors | ForEach-Object { Write-Error $_ }; Remove-Item $tmp -Force; exit 1 }; [IO.File]::Move([IO.Path]::GetFullPath($tmp), [IO.Path]::GetFullPath($Out), $true)
if ($normalizedOut -eq $defaultOut -and $normalizedGrouping -eq $defaultGrouping) { Remove-Item -LiteralPath $GroupingJson -Force }; Write-Output "wrote $Out ($($steps.Count) exact patch steps, plan_id=$(Get-Sha256Bytes ([Text.Encoding]::UTF8.GetBytes($serialized))))"
