# =====================================================================
# [ARCHIVE - Build 16] HISTORICAL COPY - DO NOT USE FOR DEPLOYMENT.
# This snapshot is kept only as a ROLLBACK REFERENCE: read-only, do not
# delete (refactor spec E.6 / P1-5).
#
# Key differences vs the current src/scripts/deploy.ps1 (V10.4.3):
#   * This V10.2 copy has NO verify-before-change gate: no expected
#     SHA256 pin and no Authenticode (signtool /pa) check BEFORE any
#     file/registry mutation.
#   * Its post-copy signature check (Patch F) is advisory only: a
#     failure prints "signature/hash verification FAILED" as a WARNING
#     and the script still ends with "Deploy complete".
#     Deploying an unverified binary while arming FilterArmed + BOOT_START
#     is the exact 0x7B unbootable scenario - which is why the current
#     version runs the hard gate FIRST.
# Use scripts/deploy.ps1 (launched via scripts/deploy.cmd) instead.
#
# This header is the ONLY change made to this snapshot file.
# =====================================================================
# deploy.ps1  (V10.2 deploy: boot-start conversion + armed rollback guard)
# Pure ASCII. Must run elevated (deploy.cmd self-elevates).
# Steps:
#   1. Clean stale PendingFileRenameOperations entries that reference RAMBlockCache.sys
#      (old reboot-replace queue is no longer needed after switching to boot-start).
#   2. Copy the signed RAMBlockCache.sys into System32\drivers. If the file is locked,
#      queue a replace-on-next-reboot via MoveFileEx (we do NOT reboot here).
#   3. Write "armed" baseline to the service root key (V10.3 single writer):
#         DeploySeq+1, BootFailCount=0, LastBootHealthy=0, FilterArmed=1
#      and ensure Features\Flags=0 (all feature flags off).
#   4. Install guard scripts into C:\ProgramData\RAMBoost.
#   5. Register two scheduled tasks:
#         RAMBoostBootGuard   (SYSTEM, AtStartup)  - auto-rollback on failed armed boot
#         RAMBoostLogonDisarm (SYSTEM, AtLogOn)    - disarm after a healthy boot
#
# This script does NOT reboot or shut down.

param(
    [string]$SysSource = ""
)

$ErrorActionPreference = 'Continue'

Write-Host '== RAMBlockCache V10.2 deploy =='

# --- 0. Admin check ---
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] Run as Administrator (launch deploy.cmd).'
    exit 1
}

$scriptsDir = $PSScriptRoot
$root       = Split-Path -Parent $scriptsDir
$paramsKey  = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$clsKey     = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'
$pdDir      = 'C:\ProgramData\RAMBoost'

# --- 1. Clean stale PendingFileRenameOperations for RAMBlockCache.sys ---
$sm = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'
$pfro = (Get-ItemProperty -Path $sm -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
if ($pfro) {
    $kept = @($pfro | Where-Object { $_ -notlike '*RAMBlockCache.sys*' })
    if ($kept.Count -gt 0) {
        Set-ItemProperty -Path $sm -Name PendingFileRenameOperations -Value $kept -Type MultiString
    } else {
        Remove-ItemProperty -Path $sm -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
    }
    Write-Host '[ok] cleaned stale PendingFileRenameOperations entries for RAMBlockCache.sys'
} else {
    Write-Host '[..] no PendingFileRenameOperations to clean'
}

# --- 2. Copy signed sys into System32\drivers ---
if (-not $SysSource) {
    $candidate = Join-Path $root 'driver\x64\Release\RAMBlockCache.sys'
    if (Test-Path $candidate) { $SysSource = $candidate }
}
$dest = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'

if ($SysSource -and (Test-Path $SysSource)) {
    try {
        Copy-Item $SysSource $dest -Force -ErrorAction Stop
        Write-Host "[ok] copied $SysSource -> $dest"
    } catch {
        Write-Host "[..] $dest is locked; queueing replace on next reboot (MoveFileEx) ..."
        if (-not ('Win32.MoveFileEx' -as [type])) {
            Add-Type -Namespace Win32 -Name MoveFileEx -MemberDefinition `
                '[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)] public static extern bool MoveFileEx(string lpExistingFileName, string lpNewFileName, int dwFlags);'
        }
        $MOVEFILE_REPLACE_EXISTING = 0x0001
        $MOVEFILE_DELAY_UNTIL_REBOOT = 0x0004
        [Win32.MoveFileEx]::MoveFileEx($SysSource, $dest, ($MOVEFILE_REPLACE_EXISTING -bor $MOVEFILE_DELAY_UNTIL_REBOOT)) | Out-Null
        Write-Host '[ok] queued replacement (takes effect on next reboot)'
    }
} else {
    Write-Host "[!] signed sys not found; skipping copy (pass -SysSource to override)."
}

# --- 2b. Convert service to BOOT_START + System Bus Extender (the V10.2 root fix) ---
& sc.exe config RAMBlockCache start= boot error= normal type= kernel group= "System Bus Extender" | Out-Host
Write-Host '[ok] service RAMBlockCache set to BOOT_START / group=System Bus Extender / ErrorControl=NORMAL'

# --- 2c. Re-append RAMBlockCache to volume-class UpperFilters (preserve volsnap) ---
$cur = (Get-ItemProperty -Path $clsKey -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
if (-not $cur) { $cur = @('volsnap') }
$cur = @($cur | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' } | Select-Object -Unique)
if ($cur -notcontains 'volsnap') { $cur = @('volsnap') + $cur }
if ($cur -notcontains 'RAMBlockCache') { $cur += 'RAMBlockCache' }
Set-ItemProperty -Path $clsKey -Name UpperFilters -Value $cur -Type MultiString
Write-Host ('[ok] UpperFilters = [' + ($cur -join ', ') + ']')

# --- 3. Write armed baseline (V10.3: single writer; setup no longer writes these) ---
#         DeploySeq/BootFailCount/LastBootHealthy/FilterArmed live under Parameters
#         (matches driver RB_REG_PARAMS = ...\RAMBlockCache\Parameters).
$svcRoot = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$seq = [int](Get-ItemProperty -Path $svcRoot -Name DeploySeq -ErrorAction SilentlyContinue).DeploySeq
Set-ItemProperty -Path $svcRoot -Name DeploySeq        -Value ($seq + 1) -Type DWord
Set-ItemProperty -Path $svcRoot -Name BootFailCount    -Value 0 -Type DWord
Set-ItemProperty -Path $svcRoot -Name LastBootHealthy  -Value 0 -Type DWord
Set-ItemProperty -Path $svcRoot -Name FilterArmed      -Value 1 -Type DWord
Write-Host ('[ok] armed: DeploySeq={0}, BootFailCount=0, LastBootHealthy=0, FilterArmed=1' -f ($seq + 1))

# --- 3b. Features\Flags default = 0 (all feature flags OFF) ---
$featKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features'
if (-not (Test-Path $featKey)) { New-Item -Path $featKey -Force | Out-Null }
Set-ItemProperty -Path $featKey -Name Flags -Value 0 -Type DWord
Write-Host '[ok] Features\Flags=0 (all feature flags off)'

# --- 4. Install guard scripts into C:\ProgramData\RAMBoost ---
if (-not (Test-Path $pdDir)) { New-Item -ItemType Directory -Path $pdDir -Force | Out-Null }
Copy-Item (Join-Path $scriptsDir 'boot_guard.ps1')    (Join-Path $pdDir 'boot_guard.ps1')    -Force
Copy-Item (Join-Path $scriptsDir 'logon_disarm.ps1')  (Join-Path $pdDir 'logon_disarm.ps1')  -Force
Write-Host "[ok] guard scripts installed to $pdDir"

# --- 5. Register scheduled tasks ---
$bgPath = Join-Path $pdDir 'boot_guard.ps1'
$ldPath = Join-Path $pdDir 'logon_disarm.ps1'

$act1 = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$bgPath`""
$trg1 = New-ScheduledTaskTrigger -AtStartup
$prin = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName 'RAMBoostBootGuard' -Action $act1 -Trigger $trg1 -Principal $prin -Force | Out-Null
Write-Host '[ok] scheduled task RAMBoostBootGuard registered (SYSTEM, AtStartup)'

$act2 = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$ldPath`""
$trg2 = New-ScheduledTaskTrigger -AtLogOn
Register-ScheduledTask -TaskName 'RAMBoostLogonDisarm' -Action $act2 -Trigger $trg2 -Principal $prin -Force | Out-Null
Write-Host '[ok] scheduled task RAMBoostLogonDisarm registered (SYSTEM, AtLogOn)'

# --- 6. Patch F: verify deployed sys signature/hash BEFORE any reboot ---
Write-Host ''
Write-Host '== Verifying deployed signature (Patch F) =='
& (Join-Path $scriptsDir 'verify_deploy_signature.ps1')
$verifyExit = $LASTEXITCODE

Write-Host ''
Write-Host '== Deploy complete. NEXT: reboot once. =='
if ($verifyExit -eq 2) {
    Write-Host '[!!] WARNING: signature/hash verification FAILED. Do NOT reboot until resolved.'
} else {
    Write-Host '  On a good boot: the logon task clears FilterArmed, passthrough stays.'
    Write-Host '  On a failed boot: the boot guard rolls UpperFilters back to volsnap-only.'
    Write-Host '  Worst case (no boot): run scripts\offline_rollback.cmd from WinRE.'
}
