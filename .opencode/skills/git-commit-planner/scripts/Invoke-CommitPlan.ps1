#Requires -Version 7
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PlanJson,
    [string]$ConfirmedPlanId,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
function Get-Sha256Bytes([byte[]]$Bytes) { ([BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($Bytes))).Replace('-', '').ToLowerInvariant() }
function Invoke-GitBytes([string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new('git'); $start.UseShellExecute = $false; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$start.ArgumentList.Add($argument) }; $process = [Diagnostics.Process]::new(); $process.StartInfo = $start; [void]$process.Start(); $stderrTask = $process.StandardError.ReadToEndAsync(); $output = [IO.MemoryStream]::new(); $process.StandardOutput.BaseStream.CopyTo($output); $process.WaitForExit(); $error = $stderrTask.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "git $($Arguments -join ' ') failed: $error" }; $output.ToArray()
}
function Invoke-Git([string[]]$Arguments) { [Text.Encoding]::UTF8.GetString((Invoke-GitBytes $Arguments)) }
function Get-PreExecutionSnapshot { [ordered]@{ head = (Invoke-Git @('rev-parse', 'HEAD')).Trim(); branch = (Invoke-Git @('symbolic-ref', '--quiet', '--short', 'HEAD')).Trim(); index_fingerprint = Get-Sha256Bytes (Invoke-GitBytes @('ls-files', '-s', '-z')) } }
function Assert-Fresh($Plan) { if (Invoke-Git @('ls-files', '-u')) { throw 'merge conflicts present — re-plan' }; $current = Get-PreExecutionSnapshot; foreach ($field in @('head', 'branch', 'index_fingerprint')) { if ($current[$field] -ne $Plan.pre_execution_snapshot.$field) { throw "$field differs from the reviewed plan; re-plan" } }; foreach ($step in @($Plan.steps)) { $literalPaths = @($step.add_paths | ForEach-Object { ":(literal)$_" }); $args = if ($step.content_mode -eq 'staged') { @('diff', '--cached', '--binary', 'HEAD', '--') } else { @('diff', '--binary', 'HEAD', '--') }; $bytes = Invoke-GitBytes ($args + $literalPaths); if ((Get-Sha256Bytes $bytes) -ne $step.payload_sha256) { throw "step $($step.index) content differs from the reviewed plan; re-plan" } } }
$builder = Join-Path $PSScriptRoot 'Build-CommitPlan.ps1'; & $builder -VerifyOnly -PlanJson $PlanJson; if ($LASTEXITCODE) { throw 'plan verification failed' }
$planBytes = [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($PlanJson)); $planId = Get-Sha256Bytes $planBytes; $plan = [Text.Encoding]::UTF8.GetString($planBytes) | ConvertFrom-Json -Depth 16; Assert-Fresh $plan
Write-Output "verified plan_id=$planId with $(@($plan.steps).Count) ordered commit(s)"
foreach ($step in @($plan.steps)) { Write-Output "step $($step.index): $($step.message.Replace("`n", ' | ')) [approved: $($step.add_paths -join ', '); touched: $($step.touched_paths -join ', ')]" }
if ($DryRun) { Write-Output 'Dry run complete; no mutation was attempted.'; exit 0 }
if ([string]::IsNullOrWhiteSpace($ConfirmedPlanId) -or $ConfirmedPlanId -cne $planId) { throw 'mutation requires -ConfirmedPlanId matching the verified plan_id' }
Assert-Fresh $plan
$tempIndex = Join-Path ([IO.Path]::GetTempPath()) ("commit-plan-$PID-$([Guid]::NewGuid().ToString('N')).index"); $oldIndex = $env:GIT_INDEX_FILE; $completed = @(); $succeeded = $false; $failure = $null
try {
    $env:GIT_INDEX_FILE = $tempIndex; [void](Invoke-GitBytes @('read-tree', $plan.pre_execution_snapshot.head))
    foreach ($step in @($plan.steps)) {
        $patchFile = "$tempIndex.$($step.index).patch"; [IO.File]::WriteAllBytes($patchFile, [Convert]::FromBase64String($step.patch_b64))
        try { [void](Invoke-GitBytes @('apply', '--cached', '--binary', '--whitespace=nowarn', $patchFile)); [void](Invoke-GitBytes @('commit', '-m', $step.message)); $completed += [pscustomobject]@{ step = $step.index; hash = (Invoke-Git @('rev-parse', 'HEAD')).Trim() } }
        finally { Remove-Item -LiteralPath $patchFile -Force -ErrorAction SilentlyContinue }
        if ($step.content_mode -eq 'all-local') { $env:GIT_INDEX_FILE = $oldIndex; $literalTouched = @($step.touched_paths | ForEach-Object { ":(literal)$_" }); [void](Invoke-GitBytes (@('restore', '--staged', '--source=HEAD', '--') + $literalTouched)); $env:GIT_INDEX_FILE = $tempIndex }
    }
    $succeeded = $true
} catch { $failure = $_ } finally { $env:GIT_INDEX_FILE = $oldIndex; if ($succeeded) { Remove-Item -LiteralPath $tempIndex -Force -ErrorAction SilentlyContinue } }
Write-Output "completed commit(s): $(($completed | ForEach-Object { "{step=$($_.step); hash=$($_.hash)}" }) -join ', ')"; Write-Output 'final status:'
try { Invoke-Git @('status', '--short', '--branch') } catch { if ($null -eq $failure) { throw }; Write-Warning "final status reporting failed: $($_.Exception.Message)" }
if ($null -ne $failure) { throw $failure }
