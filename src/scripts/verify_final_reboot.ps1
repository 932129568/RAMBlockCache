# verify_final_reboot.ps1 -- READ-ONLY acceptance after the harvest reboot.
# Implements deliverables/64-verifier-harvest-2026-11-06.md section 7.
# 2026-11-06. Run 60-90s AFTER the reboot (RbAutoAttachSweep fires ~+30s).
#
# HARD RULE (64 #7.5): only naked GET /rb/bootcfg and POST /rb/rpc method=7.
# NEVER issue method=2 (SET_CONFIG) and never use the Web settings Save button
# during this window -- either would silently zero TinyLFU / clobber bootcfg.
# method=4 (GET_CONFIG) and /rb/health are read-only and explicitly required
# by 64 #7.4, so they are included here.
#
# This script changes NOTHING: no registry writes, no config writes, no reboot.
# Exit 0 = all gates PASS, 1 = at least one FAIL, 2 = degraded (WARN only).
#
# [QA Round 2 fixes, 2026-11-06]
#   X1: registry read failure must NOT be read as "Verifier off" (fail-open).
#   X2: require a real reboot AFTER the finalize UAC (marker file), otherwise
#       "verifier /reset + no reboot" could print a false VERDICT: PASS.
#   X3: the "[630] resume: boot-critical volume" log tag drifted to 631 in the
#       current rbattach.c and is conditionally emitted (only when
#       RbIsBootCriticalPdo hits). Match by TEXT, not line number, and treat it
#       as a conditional INFO gate instead of a hard FAIL. The hard evidence
#       for D: auto-resume remains [562] + [1076] + D.Attached=1.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
# [2026-11-06 23:2x, lead] DYNAMIC sys baseline.
# The 19:22 acceptance pinned ed1bd99c... here. The 23:22 UAC rebuilt the
# driver (63ad0fa8...), so a hardcoded hash would give a permanent false FAIL
# after the reboot. Source of truth = the SIGNED dist sys; gate5 separately
# proves its three deploy anchors agree. Fail-closed if missing/unsigned.
$distSys = Join-Path $root 'dist\RAMBlockCache.sys'
$expectSys = ''
if (Test-Path -LiteralPath $distSys) {
    $ds = Get-AuthenticodeSignature -LiteralPath $distSys -ErrorAction SilentlyContinue
    if ($ds -and $ds.Status -eq 'Valid') {
        $expectSys = (Get-FileHash -LiteralPath $distSys -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower()
    }
}
$base = 'http://127.0.0.1:5199'
$markerPath = 'C:\ProgramData\rb-deploy\finalize-utc.txt'
$fails = 0
$warns = 0
function G([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Host ("[PASS] " + $name + " -> " + $detail) }
    else { Write-Host ("[FAIL] " + $name + " -> " + $detail); $script:fails++ }
}
function W([string]$name, [string]$detail) {
    Write-Host ("[WARN] " + $name + " -> " + $detail); $script:warns++
}
function I([string]$name, [string]$detail) {
    Write-Host ("[INFO] " + $name + " -> " + $detail)
}
Write-Host '== final reboot acceptance (64 #7) =='

# --- 1) boot time + X2: prove a reboot happened AFTER the finalize UAC ---
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$bootUtcStr = $boot.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture)
Write-Host ("[INFO] boot_local=" + $boot.ToString('yyyy-MM-dd HH:mm:ss') + " boot_utc=" + $bootUtcStr)
Write-Host ('[INFO] now=' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
$elapsed = (Get-Date) - $boot
if ($elapsed.TotalSeconds -lt 60) {
    W 'sampling too early' ("only " + [int]$elapsed.TotalSeconds + "s since boot; 64 #7.1 asks for 60-90s before sampling")
}
if (-not (Test-Path -LiteralPath $markerPath)) {
    G 'reboot after finalize (marker)' $false 'finalize-utc.txt missing - the RB-finalize-final.cmd UAC did not complete'
} else {
    try {
        $markerText = ([IO.File]::ReadAllText($markerPath, (New-Object Text.UTF8Encoding($false)))).Trim()
        $markerOk = ($markerText.Length -eq 24 -and $markerText.EndsWith('Z'))
        # Both are fixed-width ISO-8601 UTC -> lexicographic == chronological.
        # A boot BEFORE the marker means /reset was issued but the machine was
        # never rebooted: kernel Verifier is still on and D: resume untested.
        G 'reboot after finalize' ($markerOk -and ($bootUtcStr -gt $markerText)) ("boot_utc=" + $bootUtcStr + " finalize_utc=" + $markerText)
    } catch {
        G 'reboot after finalize (marker read)' $false $_.Exception.Message
    }
}

# --- 2) X1: Verifier must be OFF; a failed registry read is a FAIL, not a PASS ---
$mmKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
$mm = $null
try { $mm = Get-ItemProperty -LiteralPath $mmKey -ErrorAction Stop } catch { $mm = $null }
if ($null -eq $mm) {
    G 'Verifier off' $false 'Memory Management registry read failed - cannot conclude Verifier is off'
} else {
    $vd = @($mm.VerifyDrivers)
    $lvl = $mm.VerifyDriverLevel
    $vdEmpty = ($vd.Count -eq 0 -or [string]::Join(',', $vd) -eq '')
    $lvlZero = ($null -eq $lvl -or [int]$lvl -eq 0)
    G 'Verifier off' ($vdEmpty -and $lvlZero) ("VerifyDrivers=[" + [string]::Join(',', $vd) + "] VerifyDriverLevel=" + $lvl)
}

# --- 3) running driver image ---
$sys32 = 'C:\Windows\System32\drivers\RAMBlockCache.sys'
if (-not $expectSys) {
    G 'System32 sys' $false 'dist sys missing or not signed - cannot establish expected hash'
} elseif (Test-Path -LiteralPath $sys32) {
    $h = (Get-FileHash -LiteralPath $sys32 -Algorithm SHA256).Hash.ToLower()
    $sg = (Get-AuthenticodeSignature -LiteralPath $sys32).Status
    G 'System32 sys' ($h -eq $expectSys -and $sg -eq 'Valid') ("sha=" + $h + " sig=" + $sg + " expected=" + $expectSys)
} else { G 'System32 sys' $false 'file missing' }
$svc = Get-Service RAMBlockCache -ErrorAction SilentlyContinue
G 'service' ($null -ne $svc -and $svc.Status -eq 'Running' -and $svc.StartType -eq 'Boot') ("status=" + $svc.Status + " start=" + $svc.StartType)

# --- 4) watchdog full cycle + resume policy (baseline for the post-RPC recheck) ---
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$p = $null
try { $p = Get-ItemProperty -LiteralPath $k -ErrorAction Stop } catch { $p = $null }
# [2026-11-06 23:2x, lead] Structural watchdog judgement, no hardcoded Seq.
# The 19:22 cycle closed at Seq/Ack=10/10. deploy.ps1 increments DeploySeq on
# every deployment, so the 23:22 replay armed Seq=11/Ack=10; after the reboot
# the logon task disarms and Ack catches up to 11. The invariant is
#   Seq == Ack (caught up) + Armed=0 (disarmed) + Healthy=1 + BootFails=0,
# NOT a literal counter value. A failed registry read must FAIL (QA X1).
$watchOk = ($null -ne $p -and
            $null -ne $p.DeploySeq -and $null -ne $p.DeployAck -and
            [int]$p.DeploySeq -eq [int]$p.DeployAck -and
            $p.FilterArmed -eq 0 -and $p.LastBootHealthy -eq 1 -and $p.BootFailCount -eq 0)
G 'watchdog' $watchOk ("Seq=" + $p.DeploySeq + " Ack=" + $p.DeployAck + " Armed=" + $p.FilterArmed + " Healthy=" + $p.LastBootHealthy + " BootFails=" + $p.BootFailCount)
G 'bootcfg reg' ($null -ne $p -and $p.AutoAttach -eq 0 -and $p.ResumeOnBoot -eq 1 -and $p.ResumeLetters -eq 'D') ("AA=" + $p.AutoAttach + " RB=" + $p.ResumeOnBoot + " Letters=" + $p.ResumeLetters)
$seqBefore = [string]$p.DeploySeq
$ackBefore = [string]$p.DeployAck
$armedBefore = [string]$p.FilterArmed

# --- 5) new-boot log segment: [562] / [1076] hard, boot-critical message conditional ---
$logd = 'C:\ProgramData\RAMBlockCache\logs'
$cand = @()
foreach ($d in @((Get-Date), (Get-Date).AddDays(-1), (Get-Date).AddDays(1))) {
    $f = Join-Path $logd ('driver-' + $d.ToString('yyyyMMdd') + '.log')
    if (Test-Path -LiteralPath $f) { $cand += $f }
}
if ($cand.Count -eq 0) {
    G 'driver log' $false ("no driver log found in " + $logd + " for today/yesterday/tomorrow")
} else {
    $lines = @()
    foreach ($f in $cand) { $lines += @(Get-Content -LiteralPath $f) }
    # Log stamps are fixed-width ISO-8601 UTC ("2026-11-06T08:11:12.004Z",
    # exactly 24 chars), so lexicographic order == chronological order.
    # Avoid [datetime]::TryParse overload binding quirks on Windows PS 5.1.
    $cutStr = $bootUtcStr
    $seg = @($lines | Where-Object {
        $s = [string]$_
        ($s.Length -ge 24) -and ($s.Substring(0, 24) -ge $cutStr)
    })
    $has562 = [bool]($seg -match '\[562\] resume list loaded')
    $has1076 = [bool]($seg -match '\[1076\] volume attached')
    $hasBootCrit = [bool]($seg -match 'resume: boot-critical volume')
    $err = @($seg -match ' ERROR ').Count
    G 'boot [562] resume list loaded' $has562 ("segment lines=" + $seg.Count)
    G 'boot [1076] volume attached' $has1076 ("segment lines=" + $seg.Count)
    # Do NOT report "zero ERROR" as PASS when the segment is empty.
    G 'boot segment zero ERROR' ($seg.Count -gt 0 -and $err -eq 0) ("ERROR lines=" + $err + " segment lines=" + $seg.Count)
    if ($seg.Count -eq 0) { W 'boot segment' 'no lines after boot cut; log sink may lag -- recheck in 60s' }
    # X3: tag drifted 630 -> 631 and the branch is conditional. Report, do not
    # fail. The hard evidence above is what proves D: auto-resume.
    if ($hasBootCrit) { I 'boot boot-critical allowed (conditional)' 'observed in new-boot segment' }
    else { I 'boot boot-critical allowed (conditional)' 'not emitted this boot (RbIsBootCriticalPdo branch not hit / tag 630 vs 631); not a failure' }
}

# --- 6) control plane (read-only) ---
$bridgeOk = $false
try {
    $health = Invoke-RestMethod -Uri ($base + '/rb/health') -TimeoutSec 5
    $bridgeOk = $true
    G 'bridge health' $true ("pid=" + $health.pid + " node=" + $health.node + " native=" + $health.native)
    G 'bridge node v27' ([string]$health.node -match '^v?27\.') ("node=" + $health.node)
} catch {
    G 'bridge health' $false ("unreachable: " + $_.Exception.Message)
    W 'bridge fallback' 'keeper may be down; check the SYSTEM RAMBlockCacheBridge task, then re-run this script'
}
if ($bridgeOk) {
    try {
        $bc = Invoke-RestMethod -Uri ($base + '/rb/bootcfg') -TimeoutSec 5
        G 'GET /rb/bootcfg' ($bc.ResumeOnBoot -eq 1 -and $bc.ResumeLetters -eq 'D') ("AA=" + $bc.AutoAttach + " RB=" + $bc.ResumeOnBoot + " Letters=" + $bc.ResumeLetters)
    } catch { G 'GET /rb/bootcfg' $false $_.Exception.Message }
    try {
        # GET_VOLUMES decodes to { Count, Items:[{ Letter, Attached, ... }] }.
        # Use the structured shape, not a brittle regex over JSON text.
        $r7 = Invoke-RestMethod -Method Post -Uri ($base + '/rb/rpc') -ContentType 'application/json' -Body '{"method":7}' -TimeoutSec 8
        $items = @($r7.data.Items)
        $d = @($items | Where-Object { [string]$_.Letter -eq 'D' }) | Select-Object -First 1
        $att = if ($null -ne $d) { [int]$d.Attached } else { -1 }
        G 'POST /rb/rpc method=7 (D: attached)' ($null -ne $d -and $att -eq 1) ("status=" + $r7.status + " count=" + $r7.data.Count + " D.Attached=" + $att)
        # 64 #7.3: C/E/F must stay 0.
        $others = @($items | Where-Object { [string]$_.Letter -ne 'D' -and [int]$_.Attached -ne 0 })
        G 'other volumes still detached' ($others.Count -eq 0) ("non-D attached=" + $others.Count)
    } catch { G 'POST /rb/rpc method=7' $false $_.Exception.Message }
    try {
        $r4 = Invoke-RestMethod -Method Post -Uri ($base + '/rb/rpc') -ContentType 'application/json' -Body '{"method":4}' -TimeoutSec 8
        $n = @($r4.data.PSObject.Properties).Count
        $tl = $r4.data.TinyLFU
        G 'GET_CONFIG 24 fields incl TinyLFU' ($n -eq 24 -and $null -ne $tl) ("fields=" + $n + " TinyLFU=" + $tl)
    } catch { G 'GET_CONFIG' $false $_.Exception.Message }
    # 64 #7.3: the read-only RPC path must not change DeploySeq/Ack/FilterArmed.
    $p2 = Get-ItemProperty -LiteralPath $k -ErrorAction SilentlyContinue
    G 'watchdog unchanged after RPC' ($null -ne $p2 -and [string]$p2.DeploySeq -eq $seqBefore -and [string]$p2.DeployAck -eq $ackBefore -and [string]$p2.FilterArmed -eq $armedBefore) ("before Seq/Ack/Armed=" + $seqBefore + "/" + $ackBefore + "/" + $armedBefore + " after=" + $p2.DeploySeq + "/" + $p2.DeployAck + "/" + $p2.FilterArmed)
}

# --- 7) distribution artifact ---
$setup = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
if (Test-Path -LiteralPath $setup) {
    $ss = Get-AuthenticodeSignature -LiteralPath $setup
    $sh = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLower()
    G 'Setup signed' ($ss.Status -eq 'Valid') ("sha=" + $sh + " sig=" + $ss.Status)
    $rd = Join-Path $root 'README.md'
    if (Test-Path -LiteralPath $rd) {
        $hit = [bool](Select-String -LiteralPath $rd -Pattern $sh -SimpleMatch -Quiet)
        G 'README Setup hash in sync' $hit ("README contains " + $sh)
    } else { G 'README Setup hash in sync' $false 'README.md missing' }
} else { G 'Setup' $false 'dist\RAMBlockCache-Setup.exe missing' }

Write-Host ("== RESULT: FAIL=" + $fails + " WARN=" + $warns + " ==")
if ($fails -gt 0) { exit 1 }
if ($warns -gt 0) { exit 2 }
Write-Host 'VERDICT: PASS - harvest closed, Verifier off, D: auto-resumed, bridge healthy'
exit 0
