<#
.SYNOPSIS
    Idempotent WER LocalDumps provisioning for app.game.exe (Install/Check/Restore).

.DESCRIPTION
    Provisions the Windows Error Reporting LocalDumps inbox that captures full
    dumps for app.game.exe crashes:

      Key:        HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\app.game.exe
      DumpFolder: %LOCALAPPDATA%\PPR\CrashDumps\app.game.exe (REG_EXPAND_SZ, unexpanded)
      DumpCount:  4 (REG_DWORD, keeps the 4 most recent dumps)
      DumpType:   2 (REG_DWORD, full dump)

    Capture-path notes (per Microsoft WER LocalDumps documentation):
      1. HKCU LocalDumps is UNSUPPORTED. Only the HKLM LocalDumps key is
         honored by WER; a per-user key is silently ignored. This script
         therefore writes HKLM and never HKCU.
      2. An application that does its own custom crash reporting is EXCLUDED
         from LocalDumps collection. PPR ships no in-app crash reporter, and
         none must be added: this WER inbox is the ONLY supported capture
         path for app.game.exe crashes. Adding a custom unhandled-exception
         reporter would silently stop these dumps.

    The inbox directory keeps its default %LOCALAPPDATA% ACL (current user
    only). Do NOT loosen it: dumps contain process memory. WER expands
    %LOCALAPPDATA% for the crashing user at crash time, so run Install
    elevated as the same interactive user that will run the game; otherwise
    the pre-created directory lands in the wrong profile (capture still
    works -- WER creates the folder on demand -- but the pre-provisioned
    ACL check in Check mode would target the wrong path).

.PARAMETER Mode
    Install provisions the key/values and ensures the inbox directory exists,
    snapshotting any pre-existing values to the backup file first (the backup
    is only written when none exists yet, so re-installing never destroys the
    originals that Restore must put back). Check verifies values plus inbox
    writability and exits 1 on drift. Restore writes the preserved values
    back (removing the key entirely when it did not exist before Install).

.EXAMPLE
    pwsh -NoProfile -File scripts/Setup-CrashDiagnostics.ps1 -Mode Check

.EXAMPLE
    pwsh -NoProfile -File scripts/Setup-CrashDiagnostics.ps1 -Mode Install

.EXAMPLE
    pwsh -NoProfile -File scripts/Setup-CrashDiagnostics.ps1 -Mode Restore

.NOTES
    Stock pwsh 5.1 compatible. No modules. Non-interactive: never prompts.
    NEVER self-elevates: Install/Restore write HKLM, so when the session is
    not elevated they exit 2 with the exact elevated re-run command. Check
    runs as a normal user. This script never configures or builds anything.
    Exit codes: 0 success (Check: no drift), 1 Check drift / writability
    failure, 2 usage, internal, not-elevated, or missing-backup error.
    `exit` sets the process exit code for gating; do not dot-source this file.
#>
[CmdletBinding()]
param(
    [ValidateSet('Install', 'Check', 'Restore')]
    [string]$Mode = 'Check'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RegPath = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\app.game.exe'
$ExeName = 'app.game.exe'
$ExpectedFolder = '%LOCALAPPDATA%\PPR\CrashDumps\app.game.exe'
$ExpectedCount = 4
$ExpectedType = 2
$BackupPath = Join-Path $env:LOCALAPPDATA 'PPR\CrashDumps\.localdumps-backup-app.game.json'

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-InboxDir {
    return ([Environment]::ExpandEnvironmentVariables($ExpectedFolder))
}

function Read-CurrentState {
    if (-not (Test-Path -LiteralPath $RegPath)) {
        return [pscustomobject]@{ Existed = $false; Values = @{} }
    }
    $values = @{}
    foreach ($name in @('DumpFolder', 'DumpCount', 'DumpType')) {
        $prop = Get-ItemProperty -Path $RegPath -Name $name -ErrorAction SilentlyContinue
        if ($null -ne $prop) {
            $kind = (Get-Item -LiteralPath $RegPath).GetValueKind($name)
            $values[$name] = [pscustomobject]@{ Kind = "$kind"; Value = $prop.$name }
        }
    }
    return [pscustomobject]@{ Existed = $true; Values = $values }
}

function Test-InboxWritable {
    param([string]$Dir)
    try {
        if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return $false }
        $probe = Join-Path $Dir ('.writability-probe-' + [guid]::NewGuid().ToString('N'))
        [IO.File]::WriteAllText($probe, 'writable')
        Remove-Item -LiteralPath $probe -Force
        return $true
    }
    catch {
        return $false
    }
}

$exitCode = 2
try {
    if ($Mode -eq 'Install' -or $Mode -eq 'Restore') {
        if (-not (Test-IsAdmin)) {
            Write-Error ("$Mode requires elevation (HKLM write). Re-run from an elevated prompt: " +
                "Start-Process pwsh -Verb RunAs -ArgumentList '-NoProfile -File scripts/Setup-CrashDiagnostics.ps1 -Mode $Mode'. " +
                'This script never self-elevates.') -ErrorAction Continue
            $exitCode = 2
        }
        elseif ($Mode -eq 'Install') {
            if (-not (Test-Path -LiteralPath $BackupPath -PathType Leaf)) {
                $snapshot = Read-CurrentState
                $backupDir = Split-Path $BackupPath -Parent
                if (-not (Test-Path -LiteralPath $backupDir)) {
                    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
                }
                $record = [pscustomobject]@{
                    Exe        = $ExeName
                    BackedUpAt = (Get-Date).ToUniversalTime().ToString('o')
                    Existed    = $snapshot.Existed
                    Values     = $snapshot.Values
                }
                $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $BackupPath -Encoding UTF8
                Write-Host "Preserved pre-existing state to '$BackupPath' (existed: $($snapshot.Existed))."
            }
            else {
                Write-Host "Backup already exists at '$BackupPath'; keeping original pre-existing values."
            }

            if (-not (Test-Path -LiteralPath $RegPath)) {
                New-Item -Path $RegPath -Force | Out-Null
            }
            New-ItemProperty -Path $RegPath -Name 'DumpFolder' -PropertyType ExpandString -Value $ExpectedFolder -Force | Out-Null
            New-ItemProperty -Path $RegPath -Name 'DumpCount' -PropertyType DWord -Value $ExpectedCount -Force | Out-Null
            New-ItemProperty -Path $RegPath -Name 'DumpType' -PropertyType DWord -Value $ExpectedType -Force | Out-Null

            $inbox = Get-InboxDir
            if (-not (Test-Path -LiteralPath $inbox)) {
                New-Item -ItemType Directory -Force -Path $inbox | Out-Null
            }
            # NOTE: the inbox keeps its default %LOCALAPPDATA% ACL (current
            # user only). Dumps contain process memory; do not grant broader
            # access, and do not strip SYSTEM (WER writes as the crashing
            # process identity and must retain write access).
            Write-Host "Install complete: $RegPath (DumpFolder=$ExpectedFolder, DumpCount=$ExpectedCount, DumpType=$ExpectedType)."
            Write-Host "Inbox directory ensured at '$inbox' with default user-private ACL."
            $exitCode = 0
        }
        else {
            if (-not (Test-Path -LiteralPath $BackupPath -PathType Leaf)) {
                Write-Error ("No backup found at '$BackupPath'. Nothing to restore; " +
                    'Restore requires a prior Install on this machine.') -ErrorAction Continue
                $exitCode = 2
            }
            else {
                $record = Get-Content -LiteralPath $BackupPath -Raw | ConvertFrom-Json
                if (-not $record.Existed) {
                    if (Test-Path -LiteralPath $RegPath) {
                        Remove-Item -LiteralPath $RegPath -Recurse -Force
                    }
                    Write-Host 'Restore complete: key did not exist before Install; key removed.'
                }
                else {
                    if (-not (Test-Path -LiteralPath $RegPath)) {
                        New-Item -Path $RegPath -Force | Out-Null
                    }
                    foreach ($name in @('DumpFolder', 'DumpCount', 'DumpType')) {
                        $saved = $record.Values.$name
                        if ($null -ne $saved) {
                            $typeName = "$($saved.Kind)"
                            if ($typeName -eq 'ExpandString') {
                                New-ItemProperty -Path $RegPath -Name $name -PropertyType ExpandString -Value "$($saved.Value)" -Force | Out-Null
                            }
                            else {
                                New-ItemProperty -Path $RegPath -Name $name -PropertyType DWord -Value ([int]$saved.Value) -Force | Out-Null
                            }
                        }
                        else {
                            Remove-ItemProperty -Path $RegPath -Name $name -ErrorAction SilentlyContinue
                        }
                    }
                    Write-Host 'Restore complete: pre-existing values written back.'
                }
                $exitCode = 0
            }
        }
    }
    else {
        $drift = @()
        $state = Read-CurrentState
        if (-not $state.Existed) {
            $drift += "registry key missing: $RegPath (run Install from an elevated prompt)"
        }
        else {
            $folder = $state.Values['DumpFolder']
            if (($null -eq $folder) -or ("$($folder.Value)" -cne $ExpectedFolder)) {
                $drift += "DumpFolder mismatch: expected '$ExpectedFolder', found '$(if ($folder) { $folder.Value } else { '<absent>' })'"
            }
            elseif ("$($folder.Kind)" -cne 'ExpandString') {
                $drift += "DumpFolder kind mismatch: expected ExpandString, found '$($folder.Kind)'"
            }
            $count = $state.Values['DumpCount']
            if (($null -eq $count) -or ([int]$count.Value -ne $ExpectedCount)) {
                $drift += "DumpCount mismatch: expected $ExpectedCount, found '$(if ($count) { $count.Value } else { '<absent>' })'"
            }
            $dtype = $state.Values['DumpType']
            if (($null -eq $dtype) -or ([int]$dtype.Value -ne $ExpectedType)) {
                $drift += "DumpType mismatch: expected $ExpectedType, found '$(if ($dtype) { $dtype.Value } else { '<absent>' })'"
            }
        }

        $inbox = Get-InboxDir
        if (-not (Test-Path -LiteralPath $inbox -PathType Container)) {
            $drift += "inbox directory missing: '$inbox' (run Install from an elevated prompt)"
        }
        elseif (-not (Test-InboxWritable $inbox)) {
            $drift += "inbox directory not writable: '$inbox'"
        }

        foreach ($d in $drift) { Write-Host "DRIFT   $d" }
        if ($drift.Count -gt 0) {
            Write-Host ''
            Write-Host "Check FAILED: $($drift.Count) problem(s). Provision with: Start-Process pwsh -Verb RunAs -ArgumentList '-NoProfile -File scripts/Setup-CrashDiagnostics.ps1 -Mode Install'."
            $exitCode = 1
        }
        else {
            Write-Host "Check PASSED: $RegPath values correct and inbox '$inbox' writable."
            $exitCode = 0
        }
    }
}
catch {
    Write-Error "$_" -ErrorAction Continue
    $exitCode = 2
}
exit $exitCode
