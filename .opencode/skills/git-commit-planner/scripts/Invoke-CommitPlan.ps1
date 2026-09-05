#Requires -Version 7
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$PlanJson, [switch]$DryRun)
$ErrorActionPreference = 'Stop'
function Get-Sha256([string]$Text) { ([BitConverter]::ToString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLower() }
function Get-IndexFingerprint { (git write-tree).Trim() }
function Get-Patch([string]$Mode, [string[]]$Paths) {
    $literalPaths = @($Paths | ForEach-Object { ":(literal)$_" })
    if ($Mode -eq 'staged') { $p = git diff --cached --binary HEAD -- $literalPaths }
    elseif ($Mode -eq 'all-local') { $p = git diff --binary HEAD -- $literalPaths }
    else { throw "unsupported content mode: $Mode" }
    [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes((($p -join "`n") + "`n")))
}
function Get-Canonical($Steps) { @($Steps | ForEach-Object { [ordered]@{ index = $_.index; mode = $_.content_mode; paths = @($_.add_paths); patch_b64 = $_.patch_b64 } }) | ConvertTo-Json -Depth 6 -Compress }
function Assert-Fresh($Plan) {
    $porcelain = (git status --porcelain=v1 --branch) -join "`n"
    if (git ls-files -u) { throw 'merge conflicts present — clean the tree first' }
    if ($porcelain -match '(?m)^## .*no branch') { throw 'detached HEAD — clean the tree first' }
    if ((git rev-parse HEAD).Trim() -ne $Plan.head) { throw 'HEAD differs from the reviewed plan; re-plan' }
    if ((Get-IndexFingerprint) -ne $Plan.index_fingerprint) { throw 'full index differs from the reviewed plan; re-plan' }
    $current = @($Plan.steps | ForEach-Object { $copy = [pscustomobject]@{ index = $_.index; content_mode = $_.content_mode; add_paths = @($_.add_paths); patch_b64 = Get-Patch $_.content_mode @($_.add_paths) }; $copy })
    $canonical = Get-Canonical $current
    if ($canonical -ne $Plan.content_snapshot -or (Get-Sha256 $canonical) -ne $Plan.selected_content_fingerprint) { throw 'selected content differs from the reviewed plan; re-plan' }
}
Write-Output 'Commit-plan replay is user-invoked. Agents must not run this script.'
$builder = Join-Path $PSScriptRoot 'Build-CommitPlan.ps1'; & $builder -VerifyOnly -PlanJson $PlanJson; if ($LASTEXITCODE) { throw 'plan verification failed' }
$plan = Get-Content $PlanJson -Raw | ConvertFrom-Json -Depth 12; Assert-Fresh $plan
Write-Output 'Replay uses a temporary Git index; stored patch payloads are the only replay content.'
foreach ($step in @($plan.steps)) { Write-Output "step $($step.index): git apply --cached --binary <stored patch payload>"; Write-Output "step $($step.index): git commit -m <reviewed message>" }
if ($DryRun) { exit 0 }
Write-Host 'Type COMMIT to apply the reviewed payloads through a temporary index'
if ([Console]::In.ReadLine() -cne 'COMMIT') { Write-Output 'Replay cancelled before mutation.'; exit 0 }
Assert-Fresh $plan
$tempIndex = Join-Path (Join-Path $env:LOCALAPPDATA 'Temp\opencode') ("commit-plan-$PID-$([Guid]::NewGuid().ToString('N')).index")
$oldIndex = $env:GIT_INDEX_FILE
try {
    $env:GIT_INDEX_FILE = $tempIndex; & git read-tree $plan.head; if ($LASTEXITCODE) { throw 'temporary index initialization failed' }
    foreach ($step in @($plan.steps)) {
        $patch = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($step.patch_b64)); $patchFile = "$tempIndex.$($step.index).patch"; [IO.File]::WriteAllText($patchFile, $patch, [Text.UTF8Encoding]::new($false))
        & git apply --cached --binary --whitespace=nowarn $patchFile; if ($LASTEXITCODE) { throw "step $($step.index) apply failed; temporary index retained at $tempIndex; no reset was run" }
        & git commit -m $step.message; if ($LASTEXITCODE) { throw "step $($step.index) commit failed; temporary index retained at $tempIndex; no reset was run" }
        if ($step.content_mode -eq 'all-local') {
            $env:GIT_INDEX_FILE = $oldIndex
            $literalTouched = @($step.touched_paths | ForEach-Object { ":(literal)$_" })
            & git restore --staged --source=HEAD -- $literalTouched
            if ($LASTEXITCODE) { throw "step $($step.index) committed but selected-index reconciliation failed; inspect selected paths manually" }
            $env:GIT_INDEX_FILE = $tempIndex
        }
        Remove-Item $patchFile -Force
    }
    Remove-Item $tempIndex -Force
} finally { $env:GIT_INDEX_FILE = $oldIndex }
