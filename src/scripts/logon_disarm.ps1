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
