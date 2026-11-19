# boot_guard.ps1  [V10.4 / 88 M-6] OnStartup guard.
# Normal path: roll back UpperFilters only when armed AND a boot failure is
# confirmed. Extra path (88 M-6): if FilterArmed was NEVER set but the
# Volume class UpperFilters already references RAMBlockCache, that is the
# B-2 historical half-installed state (engine appended the filter and then
# returned before arming). Remove the dangling reference so the next boot
# cannot 0x7B on an unloadable driver.
$svc      = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$volClass = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'
# [TICKET 2026-11-16] "This boot reached user mode" ticket -> Parameters\BootReachedUser.
# Consumed once by the driver on the NEXT boot (RbBootFailLoad):
#   if (healthy == 1 || (ticket == 1 && clean == 1)) { fails = 0; }   ticket read, then cleared.
# G1: the driver really is loaded this boot (service Running) -- a 0x7B / unloaded boot never
#     reaches this AtStartup task. G2: the volume class still lists RAMBlockCache -- blocks the
#     white ticket of a boot where the driver runs but never attaches (half-install / rolled back).
# Call ONLY on the "not a confirmed failure" exits below; NEVER on the two UpperFilters strip
# paths, otherwise the next boot erases the failure this very boot just recorded.
function Set-BootReachedUserTicket {
    param([string]$Key, [bool]$HasRb)
    if (-not $HasRb) { return }
    $st = (Get-Service -Name RAMBlockCache -ErrorAction SilentlyContinue).Status
    if ("$st" -ne 'Running') { return }
    try { Set-ItemProperty -Path $Key -Name BootReachedUser -Value 1 -Type DWord -ErrorAction SilentlyContinue } catch { }
}
try {
    $p = Get-ItemProperty -Path $svc -ErrorAction Stop
    $armed = ([int]$p.FilterArmed -eq 1)
    $uf = (Get-ItemProperty -Path $volClass -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
    $hasRb = $false
    if ($uf) { $hasRb = @($uf | Where-Object { $_ -eq 'RAMBlockCache' }).Count -gt 0 }

    if (-not $armed) {
        if (-not $hasRb) { exit 0 }   # not armed and no dangling reference: nothing to repair
        # [FIX 2026-11-13 J6-ROOT] A healthy, already-acknowledged deployment
        # is NOT an unarmed half-install. logon_disarm.ps1 clears FilterArmed
        # on every successful logon AND writes DeployAck = DeploySeq; a real
        # B-2 half-install has DeploySeq > DeployAck (deploy.ps1 bumped the
        # sequence before it bailed out). Stripping UpperFilters in the healthy
        # case removed the volume class filter on the NEXT boot, so PnP never
        # called RbAddDevice: DevList empty -> volumes:0 -> no cache IO ->
        # L2 never opened and H5 never received the power IRP that finalises
        # the L2 index. Real incident 2026-11-13: watch history shows
        # [volsnap,RAMBlockCache] at 11:37 and [volsnap] at 15:13.
        $seq = [int]$p.DeploySeq
        $ack = [int]$p.DeployAck
        # [TICKET] healthy, already-acked deployment: issue the ticket (mirrors the exit guard
        # below); the strip path after it must not issue one.
        if ($seq -gt 0 -and $seq -eq $ack) { Set-BootReachedUserTicket -Key $svc -HasRb $hasRb }
        if ($seq -gt 0 -and $seq -eq $ack) { exit 0 }
        # [88 M-6] genuine unarmed half-install: strip the dangling filter.
        $keep = @($uf | Where-Object { $_ -ne 'RAMBlockCache' })
        if ($keep.Count -eq 0) { $keep = @('volsnap') }
        Set-ItemProperty -Path $volClass -Name UpperFilters -Value $keep -Type MultiString
        Set-ItemProperty -Path $svc -Name FilterArmed -Value 0 -Type DWord
        exit 0
    }
    # [TICKET] armed and no confirmed failure: issue the ticket. The guard is duplicated from the
    # exit below on purpose -- the rollback path past it must not issue a ticket.
    if ([int]$p.BootFailCount -lt 1) { Set-BootReachedUserTicket -Key $svc -HasRb $hasRb }
    if ([int]$p.BootFailCount -lt 1) { exit 0 }   # no confirmed failure: do not touch

    if ($uf) {
        $keep = @($uf | Where-Object { $_ -ne 'RAMBlockCache' })
        if ($keep.Count -eq 0) { $keep = @('volsnap') }
        Set-ItemProperty -Path $volClass -Name UpperFilters -Value $keep -Type MultiString
    }
    Set-ItemProperty -Path $svc -Name FilterArmed -Value 0 -Type DWord
    exit 0
} catch { exit 1 }
