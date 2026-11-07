# boot_guard.ps1  [V10.4 / 88 M-6] OnStartup guard.
# Normal path: roll back UpperFilters only when armed AND a boot failure is
# confirmed. Extra path (88 M-6): if FilterArmed was NEVER set but the
# Volume class UpperFilters already references RAMBlockCache, that is the
# B-2 historical half-installed state (engine appended the filter and then
# returned before arming). Remove the dangling reference so the next boot
# cannot 0x7B on an unloadable driver.
$svc      = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$volClass = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'
try {
    $p = Get-ItemProperty -Path $svc -ErrorAction Stop
    $armed = ([int]$p.FilterArmed -eq 1)
    $uf = (Get-ItemProperty -Path $volClass -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
    $hasRb = $false
    if ($uf) { $hasRb = @($uf | Where-Object { $_ -eq 'RAMBlockCache' }).Count -gt 0 }

    if (-not $armed) {
        if (-not $hasRb) { exit 0 }   # not armed and no dangling reference: nothing to repair
        # [88 M-6] unarmed half-install: strip the dangling filter immediately.
        $keep = @($uf | Where-Object { $_ -ne 'RAMBlockCache' })
        if ($keep.Count -eq 0) { $keep = @('volsnap') }
        Set-ItemProperty -Path $volClass -Name UpperFilters -Value $keep -Type MultiString
        Set-ItemProperty -Path $svc -Name FilterArmed -Value 0 -Type DWord
        exit 0
    }
    if ([int]$p.BootFailCount -lt 1) { exit 0 }   # no confirmed failure: do not touch

    if ($uf) {
        $keep = @($uf | Where-Object { $_ -ne 'RAMBlockCache' })
        if ($keep.Count -eq 0) { $keep = @('volsnap') }
        Set-ItemProperty -Path $volClass -Name UpperFilters -Value $keep -Type MultiString
    }
    Set-ItemProperty -Path $svc -Name FilterArmed -Value 0 -Type DWord
    exit 0
} catch { exit 1 }
