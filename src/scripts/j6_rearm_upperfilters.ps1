# j6_rearm_upperfilters.ps1
# Restore the volume-class UpperFilters reference removed by boot_guard.ps1.
# User-approved 2026-11-13 (path C). Run ELEVATED via RB-j6-rearm-upperfilters.cmd.
#
# PREFLIGHT IS FAIL-CLOSED: no registry write happens unless all of these hold:
#   - System32 sys sha256 equals the signed deployment image
#   - Authenticode status is Valid
#   - service Start is 0 (BOOT_START)
# ACTION: append RAMBlockCache to the volume class UpperFilters (preserving
# volsnap and every other filter, same logic as deploy.ps1:442-448), then set
# Parameters\FilterArmed = 1. Every step is read back and verified.
# NO sys replacement, NO sign, NO anchor change, NO reboot, NO write-back or
# WBCOAL change, NO method=2, NO bootcfg POST, NO verifier /reset.
$ErrorActionPreference = 'Stop'

$sys      = 'C:\Windows\System32\drivers\RAMBlockCache.sys'
$expected = '7125d2199981177925b2fbb9d2e6a055ab65895afadca98ca6ac0040c7cded47'
$cls      = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'
$svcKey   = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache'
$parm     = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$log      = 'C:\ProgramData\rb-deploy\j6-rearm-upperfilters.log'

function Note([string]$m) {
    Write-Host $m
    Add-Content -LiteralPath $log -Value $m -Encoding UTF8
}

Note ('=== REARM ' + (Get-Date).ToUniversalTime().ToString('o') + ' ===')

# ---- preflight (fail closed, zero writes before this point) ----
if (-not (Test-Path -LiteralPath $sys)) { throw 'System32 sys missing' }
$h = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash.ToLower()
if ($h -ne $expected) { throw ('sys sha mismatch: ' + $h) }
$sig = (Get-AuthenticodeSignature -LiteralPath $sys -ErrorAction Stop).Status.ToString()
if ($sig -ne 'Valid') { throw ('Authenticode not Valid: ' + $sig) }
$start = (Get-ItemProperty -LiteralPath $svcKey -Name Start -ErrorAction Stop).Start
if ([int]$start -ne 0) { throw ('service Start is not BOOT_START: ' + $start) }
Note ('PREFLIGHT OK sha=' + $h + ' sig=' + $sig + ' start=' + $start)

# ---- backup, append, read back ----
$uf = @((Get-ItemProperty -LiteralPath $cls -Name UpperFilters -ErrorAction Stop).UpperFilters)
$before = $uf -join ','
$cur = @($uf | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' } | Select-Object -Unique)
if ($cur -notcontains 'volsnap') { $cur = @('volsnap') + $cur }
if ($cur -notcontains 'RAMBlockCache') { $cur += 'RAMBlockCache' }
$after = $cur -join ','
if ($before -eq $after) {
    Note ('NO CHANGE: UpperFilters already [' + $before + ']')
} else {
    Note ('BACKUP UpperFilters=[' + $before + ']')
    Set-ItemProperty -LiteralPath $cls -Name UpperFilters -Value $cur -Type MultiString
    Note ('SET    UpperFilters=[' + $after + ']')
}
$readback = (@((Get-ItemProperty -LiteralPath $cls -Name UpperFilters -ErrorAction Stop).UpperFilters) -join ',')
if ($readback -ne $after) { throw ('UpperFilters readback mismatch: ' + $readback) }

Set-ItemProperty -LiteralPath $parm -Name FilterArmed -Value 1 -Type DWord
$armed = (Get-ItemProperty -LiteralPath $parm -Name FilterArmed -ErrorAction Stop).FilterArmed
if ([int]$armed -ne 1) { throw 'FilterArmed readback failed' }

Note ('VERIFIED UpperFilters=[' + $readback + '] FilterArmed=' + $armed)
Note 'ROLLBACK: remove RAMBlockCache from class UpperFilters; set FilterArmed=0'
Note 'NEXT: reboot ONCE from Start menu, then run RB-j6-postboot-probe.cmd again'
Write-Host 'REARM OK'
