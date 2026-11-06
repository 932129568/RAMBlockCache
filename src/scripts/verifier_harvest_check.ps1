# verifier_harvest_check.ps1 -- READ-ONLY pre-harvest gate for Driver Verifier cycle
# 2026-11-06 PF-16 batch. Pure ASCII + UTF-8 BOM (check_ps_syntax gate 4).
# Usage: powershell -ExecutionPolicy Bypass -File src\scripts\verifier_harvest_check.ps1 [-Hours 1]
# It never changes registry, services, files or Verifier settings.
param([int]$Hours = 1)
$ErrorActionPreference = 'Continue'

Write-Host '== RAMBlockCache Verifier harvest pre-check (READ ONLY) =='
Write-Host ("checked at : " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Write-Host ("window     : last " + $Hours + " hour(s)")

# --- 1) which binary is actually on disk / running ---
$os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
if ($os) { Write-Host ("last boot   : " + $os.LastBootUpTime) }
$sys = Join-Path $env:windir 'System32\drivers\RAMBlockCache.sys'
if (Test-Path -LiteralPath $sys) {
    $h = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash.ToLower()
    $sg = Get-AuthenticodeSignature -LiteralPath $sys
    Write-Host ("System32 sys: " + $h)
    Write-Host ("signature   : " + $sg.Status)
} else {
    Write-Host 'System32 sys: MISSING'
}
$svc = Get-Service RAMBlockCache -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host ("service     : " + $svc.Status + " / " + $svc.StartType)
} else {
    Write-Host 'service     : MISSING'
}
$p = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters' -ErrorAction SilentlyContinue
if ($p) {
    Write-Host ("guard       : Seq=" + $p.DeploySeq + " Ack=" + $p.DeployAck + " Armed=" + $p.FilterArmed + " Healthy=" + $p.LastBootHealthy + " BootFails=" + $p.BootFailCount)
}

# --- 2) Verifier state ---
$mm = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management' -ErrorAction SilentlyContinue
Write-Host ("VerifyDrivers: [" + $mm.VerifyDrivers + "]")
Write-Host ("VerifyType   : [" + $mm.VerifyDriversType + "]")

# --- 3) crash evidence in the window ---
$since = (Get-Date).AddHours(-$Hours)
$evts = @()
try {
    $evts = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since; Id = 1001,41,6008 } -ErrorAction Stop)
} catch {
    Write-Host ("event query : no matching events / channel unavailable (" + $_.Exception.Message + ")")
}
Write-Host ("bugcheck/power/unexpected-shutdown events: " + $evts.Count)
foreach ($e in ($evts | Select-Object -First 8)) {
    Write-Host ("  " + $e.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss') + " id=" + $e.Id + " src=" + $e.ProviderName)
}
$whea = @()
try {
    $whea = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since; ProviderName = 'Microsoft-Windows-WHEA-Logger' } -ErrorAction Stop)
} catch {
    $whea = @()
}
Write-Host ("WHEA hardware-error events: " + $whea.Count)

# --- 4) dumps produced in the window ---
$dumpList = @()
$md = Join-Path $env:windir 'Minidump'
if (Test-Path -LiteralPath $md) {
    $dumpList += @(Get-ChildItem -LiteralPath $md -Filter '*.dmp' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $since })
}
$memDump = Join-Path $env:windir 'MEMORY.DMP'
if (Test-Path -LiteralPath $memDump) {
    $mi = Get-Item -LiteralPath $memDump
    if ($mi.LastWriteTime -ge $since) { $dumpList += $mi }
}
Write-Host ("new dumps: " + $dumpList.Count)
foreach ($d in $dumpList) {
    Write-Host ("  " + $d.FullName + "  " + $d.Length + " B  " + $d.LastWriteTime)
}

# --- 5) human-readable gate hint (never auto-disables anything) ---
if ($evts.Count -gt 0 -or $whea.Count -gt 0 -or $dumpList.Count -gt 0) {
    Write-Host 'VERDICT: NOT CLEAN - crash evidence in window. DO NOT disable Verifier yet.'
    Write-Host '  Collect dumps, then run cdb -z <dump> -c "!analyze -v; ln rbc+0xBCC; q"'
    Write-Host '  with the current build PDB for exact attribution.'
} else {
    Write-Host 'VERDICT: no crash evidence in window.'
    Write-Host '  If the 1h daily-load cycle is COMPLETE, run verifier_disable.cmd then reboot.'
    Write-Host '  If the boot shown above predates the PF-16 deploy, reboot once first: the'
    Write-Host '  running image is still the old driver, so this hour did NOT cover the new build.'
}
exit 0
