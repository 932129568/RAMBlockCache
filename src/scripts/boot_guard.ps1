# boot_guard.ps1  [V10.3] OnStartup guard: only roll back UpperFilters when armed AND a boot failure is confirmed
$svc      = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$volClass = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'
try {
    $p = Get-ItemProperty -Path $svc -ErrorAction Stop
    if ([int]$p.FilterArmed -ne 1) { exit 0 }     # not armed: do not touch
    if ([int]$p.BootFailCount -lt 1) { exit 0 }   # no confirmed failure: do not touch (grace/healthy boots no longer false-trigger)

    $uf = (Get-ItemProperty -Path $volClass -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
    if ($uf) {
        $keep = @($uf | Where-Object { $_ -ne 'RAMBlockCache' })
        if ($keep.Count -eq 0) { $keep = @('volsnap') }
        Set-ItemProperty -Path $volClass -Name UpperFilters -Value $keep -Type MultiString
    }
    Set-ItemProperty -Path $svc -Name FilterArmed -Value 0 -Type DWord
    exit 0
} catch { exit 1 }
