# =====================================================================
# [ARCHIVE - Build 16] HISTORICAL COPY - DO NOT USE.
# This file is byte-identical to the current src/scripts/logon_disarm.ps1
# (V10.3); it is kept only as part of the Build 16 rollback-reference
# snapshot: read-only, do not delete (refactor spec E.6 / P1-5).
# Use the copy under src/scripts/.
#
# This header is the ONLY change made to this snapshot file.
# =====================================================================
# logon_disarm.ps1  [V10.3] healthy desktop reached -> write health latch + disarm
$svc = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
try {
    $p = Get-ItemProperty -Path $svc -ErrorAction Stop
    $seq = [int]$p.DeploySeq
    if ($seq -gt 0) {
        Set-ItemProperty -Path $svc -Name DeployAck      -Value $seq -Type DWord
    }
    Set-ItemProperty -Path $svc -Name LastBootHealthy -Value 1 -Type DWord
    Set-ItemProperty -Path $svc -Name FilterArmed     -Value 0 -Type DWord
    exit 0
} catch { exit 1 }
