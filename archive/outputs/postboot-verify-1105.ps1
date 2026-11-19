# Postboot verification after deploying signed c4531251 (D391DD55 + BOOTCFG)
# Run in USER context after reboot. Read-only.
$ErrorActionPreference = 'Continue'
$expect = 'c4531251'
$fail = 0

Write-Host '== 1. Driver service state =='
$s = sc.exe query RAMBlockCache
$s | ForEach-Object { Write-Host "  $_" }
if (($s | Select-String 'RUNNING')) { Write-Host '  OK: RUNNING' } else { Write-Host '  FAIL: not RUNNING'; $fail++ }

Write-Host '== 2. Loaded image identity (BOOT_START reads disk at boot) =='
$svc = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache').ImagePath
$sys = Join-Path $env:SystemRoot ($svc -replace '^\\SystemRoot\\','')
$h = (Get-FileHash $sys -Algorithm SHA256).Hash.ToLower().Substring(0,8)
$sig = (Get-AuthenticodeSignature $sys).Status
Write-Host "  file: $sys"
Write-Host "  hash: $h (expect $expect)  signature: $sig"
if ($h -eq $expect -and $sig -eq 'Valid') { Write-Host '  OK: new signed image on disk' } else { Write-Host '  FAIL: image mismatch'; $fail++ }

Write-Host '== 3. Boot policy registry (driver reads at DriverEntry) =='
$p = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
foreach ($k in 'AutoAttach','ResumeOnBoot','WriteBack','CacheSizeMB','BlockSize') {
    Write-Host ("  {0,-13} = {1}" -f $k, $p.$k)
}
$ok = ($p.AutoAttach -eq 0) -and ($p.ResumeOnBoot -eq 1) -and ($p.WriteBack -eq 0) -and ($p.CacheSizeMB -eq 0) -and ($p.BlockSize -eq 4096)
if ($ok) { Write-Host '  OK: AutoAttach=0 ResumeOnBoot=1 WriteBack=0 CacheSizeMB=0 BlockSize=4096' } else { Write-Host '  FAIL: registry drifted'; $fail++ }

Write-Host ''
if ($fail -eq 0) {
    Write-Host 'BASELINE OK. Manual checks (web console):'
    Write-Host '  a. Settings page "boot behavior" group shows two switches: AutoAttach off / ResumeOnBoot on'
    Write-Host '     -> if visible and toggleable, IOCTL_RB_BOOTCFG read+write works'
    Write-Host '  b. Volumes page: D: should be auto-resumed (ResumeOnBoot=1), no other volume attached'
} else {
    Write-Host "BASELINE FAILURES: $fail"
}
