# j6_deploy_guard_fix.ps1
# Deploy the fixed boot_guard.ps1 to the LIVE runtime location, then re-arm.
# User-approved 2026-11-13 (path C, ds-unstick round 2). Run ELEVATED.
#
# ROOT CAUSE OF THE PREVIOUS FAILED FIX: src/scripts/boot_guard.ps1 was fixed
# in the repo, but the scheduled task RAMBoostBootGuard actually runs
# C:\ProgramData\RAMBoost\boot_guard.ps1 (deploy.ps1:497 copies it there).
# The live copy was still the old V10.4 script, so it kept stripping
# RAMBlockCache from UpperFilters on every healthy-disarmed boot.
#
# PREFLIGHT IS FAIL-CLOSED (no write before all checks pass):
#   - running sys sha / Authenticode / BOOT_START
#   - the REPO copy actually contains the DeployAck guard (never deploy an
#     unfixed script over the live one)
# ACTION: back up live guard -> copy repo guard -> verify marker present in
# the live copy -> append RAMBlockCache to UpperFilters -> FilterArmed=1 ->
# read everything back. Log contains the exact rollback instructions.
$ErrorActionPreference = 'Stop'

$sys       = 'C:\Windows\System32\drivers\RAMBlockCache.sys'
$expected  = '7125d2199981177925b2fbb9d2e6a055ab65895afadca98ca6ac0040c7cded47'
$svcKey    = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache'
$parm      = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$cls       = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'
$repoGuard = Join-Path $PSScriptRoot 'boot_guard.ps1'
$liveGuard = 'C:\ProgramData\RAMBoost\boot_guard.ps1'
$backup    = $liveGuard + '.j6bak'
$log       = 'C:\ProgramData\rb-deploy\j6-deploy-guard-fix.log'
$marker    = '$seq -gt 0 -and $seq -eq $ack'

function Note([string]$m) {
    Write-Host $m
    Add-Content -LiteralPath $log -Value $m -Encoding UTF8
}

Note ('=== DEPLOY GUARD FIX ' + (Get-Date).ToUniversalTime().ToString('o') + ' ===')

# ---- preflight (zero writes) ----
if (-not (Test-Path -LiteralPath $sys)) { throw 'System32 sys missing' }
$h = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash.ToLower()
if ($h -ne $expected) { throw ('sys sha mismatch: ' + $h) }
$sig = (Get-AuthenticodeSignature -LiteralPath $sys -ErrorAction Stop).Status.ToString()
if ($sig -ne 'Valid') { throw ('Authenticode not Valid: ' + $sig) }
$start = (Get-ItemProperty -LiteralPath $svcKey -Name Start -ErrorAction Stop).Start
if ([int]$start -ne 0) { throw ('service Start is not BOOT_START: ' + $start) }
if (-not (Test-Path -LiteralPath $repoGuard)) { throw 'repo boot_guard.ps1 missing' }
$repoText = [IO.File]::ReadAllText($repoGuard)
if (-not $repoText.Contains($marker)) { throw 'repo boot_guard.ps1 is NOT the fixed version' }
if (-not (Test-Path -LiteralPath $liveGuard)) { throw 'live boot_guard.ps1 missing' }
Note ('PREFLIGHT OK sha=' + $h + ' sig=' + $sig + ' start=' + $start)

# ---- deploy the guard ----
Copy-Item -LiteralPath $liveGuard -Destination $backup -Force
Note ('BACKUP live guard -> ' + $backup)
Copy-Item -LiteralPath $repoGuard -Destination $liveGuard -Force
$liveText = [IO.File]::ReadAllText($liveGuard)
if (-not $liveText.Contains($marker)) { throw 'live guard missing fix marker after copy' }
Note 'VERIFIED live boot_guard.ps1 contains the DeployAck guard'

# ---- re-arm the volume class filter ----
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

Note 'ROLLBACK: restore C:\ProgramData\RAMBoost\boot_guard.ps1.j6bak; remove RAMBlockCache from class UpperFilters; set FilterArmed=0'
Note 'NEXT: 1) RB-l2-j6-prepare.cmd MUST print FIXTURE-PASS and "L2 left ON"'
Note '      2) ONE normal Start-menu reboot (this is where the fixed guard is exercised: FilterArmed=0 at boot)'
Note '      3) RB-l2-j6-verify.cmd, then RB-l2-p0c-rollback.cmd'
Write-Host 'DEPLOY GUARD FIX OK'
