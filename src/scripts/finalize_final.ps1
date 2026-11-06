# finalize_final.ps1 -- ONE UAC to finish the Verifier 1h harvest cycle.
# 2026-11-06. Log: C:\ProgramData\rb-deploy\finalize-final.log
#
# WHY THIS EXISTS: RB-finalize-harvest.cmd (18:04) stopped after the vite build
# step. In an elevated Windows PowerShell 5.1 session with
# $ErrorActionPreference='Stop', native stderr redirected through 2>&1 becomes
# a terminating NativeCommandError and aborts the pipeline. The rebuild itself
# was subsequently completed successfully in a NORMAL session at 18:08:39,
# producing an UNSIGNED dist\RAMBlockCache-Setup.exe
# (8c5cf32899316d2ff41bb3255b89091d8a1f65a65a3e202de8d3d3e40170ae4c,
#  276,151,410 B) whose payload was verified to carry:
#   - the #3 TinyLFU config-chain fix (decodeConfig / encodeParams 156B)
#   - the #6 launcher path fix (bridge\server\index.ts)
#   - Node 27 + npm 12.2.1 built in, dev-only target/ and .mimosa/ stripped
# So this script does NOT rebuild. It is idempotent and only:
#   0) assert sys is still the signed ed1bd99c... baseline (no anchor drift)
#   1) sign Setup on an ASCII stage path, copy back, verify Valid
#      (skipped cleanly if Setup is already Valid)
#   2) gate5 (sys signature chain + three anchors) as a CHILD PROCESS so the
#      exit code is unambiguous and Write-Host output is capturable
#   3) verifier /reset (takes effect after reboot)
#   4) refresh the rb-deploy staging copy + record final hashes
$ErrorActionPreference = 'Continue'
$global:LASTEXITCODE = 0
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated'
    exit 1
}
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logDir  = 'C:\ProgramData\rb-deploy'
$log     = Join-Path $logDir 'finalize-final.log'
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$psExe   = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$verExe  = Join-Path $env:WINDIR 'System32\verifier.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'
$setup   = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
$sys     = Join-Path $root 'dist\RAMBlockCache.sys'
$stage   = Join-Path $logDir 'RAMBlockCache-Setup.exe'
$gate    = Join-Path $root 'src\scripts\check_signature.ps1'
$expectSys = 'ed1bd99cd56812a36c2adb2e6f60f79b87512886bd2d02214eb25840663ab68e'
function L([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m)
}
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
'' | Set-Content -LiteralPath $log
foreach ($f in @($sigtool, $psExe, $verExe, $setup, $sys, $gate)) {
    if (-not (Test-Path -LiteralPath $f)) { L ("MISSING: " + $f); exit 1 }
}
Set-Location -LiteralPath $root
L ("start: setup_size=" + (Get-Item -LiteralPath $setup).Length)

# --- 0) fail-closed baseline: sys must be the signed, anchored image ---
$sysHash0 = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash.ToLower()
if ($sysHash0 -ne $expectSys) { L ("SYS BASELINE MISMATCH: " + $sysHash0); exit 1 }
L ("[0] sys baseline OK sha256=" + $sysHash0)
$bc = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
if (Test-Path -LiteralPath $bc) {
    $p = Get-ItemProperty -LiteralPath $bc
    L ("[0] bootcfg before: AutoAttach=" + $p.AutoAttach + " ResumeOnBoot=" + $p.ResumeOnBoot + " ResumeLetters=" + $p.ResumeLetters)
}
$mm = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
$vp = Get-ItemProperty -LiteralPath $mm -ErrorAction SilentlyContinue
L ("[0] verifier before: VerifyDrivers=" + ($vp.VerifyDrivers -join ','))

# [QA H fix, 2026-11-06] FAIL-OPEN closure.
# The ASCII stage C:\ProgramData\rb-deploy\RAMBlockCache-Setup.exe currently
# holds the OLD 15:50 signed Setup (7912d276...), which does NOT contain the
# #3 TinyLFU config-chain fix or the #6 launcher path fix. With EAP='Continue',
# a non-terminating Copy-Item failure (AV lock / handle on a 276MB exe) would
# leave that stale file in place; signtool would re-sign it, it would be copied
# back over dist, and ALL downstream gates would still pass because they only
# verify the sys hash. Pin the payload identity before signing and prove the
# stage was really overwritten. Both checks are read-only Get-FileHash.
$expectSetup = '8c5cf32899316d2ff41bb3255b89091d8a1f65a65a3e202de8d3d3e40170ae4c'
$staleSetup  = '7912d2760efc4249aafd6269a22e2e40fa1bdda39cdd4f888bc48d63c30867af'
$setupHash0 = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLower()
L ("[0] setup sha256=" + $setupHash0)
if ($setupHash0 -eq $staleSetup) { L 'SETUP IS THE STALE OLD 15:50 BUILD - abort'; exit 1 }

# --- 1) sign (idempotent: skip if already Valid) ---
$cur = Get-AuthenticodeSignature -LiteralPath $setup
L ("[1] current Setup signature status=" + $cur.Status)
if ($cur.Status -ne 'Valid') {
    if ($setupHash0 -ne $expectSetup) { L ("SETUP BASELINE MISMATCH: " + $setupHash0); exit 1 }
    L '[1] setup baseline OK - rebuilt unsigned payload with #3/#6 fixes'
    Copy-Item -LiteralPath $setup -Destination $stage -Force
    $stageHash0 = (Get-FileHash -LiteralPath $stage -Algorithm SHA256).Hash.ToLower()
    if ($stageHash0 -ne $setupHash0) { L ("STAGE COPY MISMATCH: " + $stageHash0); exit 1 }
    L '[1] stage overwritten OK, signing ...'
    $signOut = & $sigtool sign /sm /s My /sha1 $thumb /fd SHA256 $stage 2>&1
    $rc = $LASTEXITCODE
    $signOut | ForEach-Object { L ("  signtool: " + $_) }
    if ($rc -ne 0) { L ("SETUP SIGN FAILED exit=" + $rc); exit 1 }
    $sp = Get-AuthenticodeSignature -LiteralPath $stage
    if ($sp.Status -ne 'Valid') { L ("SETUP SIGNATURE " + $sp.Status); exit 1 }
    $signedStageHash = (Get-FileHash -LiteralPath $stage -Algorithm SHA256).Hash.ToLower()
    Copy-Item -LiteralPath $stage -Destination $setup -Force
    $copyBackHash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLower()
    if ($copyBackHash -ne $signedStageHash) { L ("SIGNED COPY-BACK MISMATCH: " + $copyBackHash); exit 1 }
    L '[1] signed OK'
} else {
    L '[1] already signed, skip (verified not the stale old build)'
}
$finalSig = Get-AuthenticodeSignature -LiteralPath $setup
if ($finalSig.Status -ne 'Valid') { L ("SETUP NOT VALID: " + $finalSig.Status); exit 1 }
$setupHash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLower()
L ("[1] final setup sha256=" + $setupHash + " size=" + (Get-Item -LiteralPath $setup).Length)

# --- 2) gate5 as a child process (reliable exit code + capturable output) ---
L '[2] gate5 ...'
$gOut = & $psExe -NoProfile -ExecutionPolicy Bypass -File $gate 2>&1
$grc = $LASTEXITCODE
$gOut | ForEach-Object { L ("  gate5: " + $_) }
L ("[2] gate5 exit=" + $grc)
$gatePass = [bool]($gOut -match 'result: PASS')
if ($grc -ne 0 -or -not $gatePass) { L 'GATE5 FAILED (exit code or result text)'; exit 1 }

# --- 3) verifier /reset ---
# [19:05 UAC fix] verifier.exe returns 2 when a REBOOT IS REQUIRED to apply the
# reset (its output ends with "需要重新启动系统才能使设置生效"). That is a success
# outcome, not a failure. The 19:05 run aborted here on exit=2 even though the
# registry had already been cleared, so the reboot marker and hash ledger were
# never written. Per QA Round 2 X2, an exit code alone is never authoritative:
# assert the registry state instead, fail-closed on any read failure.
L '[3] verifier /reset ...'
$vOut = & $verExe /reset 2>&1
$vrc = $LASTEXITCODE
$vOut | ForEach-Object { L ("  verifier: " + $_) }
L ("[3] verifier reset exit=" + $vrc)
if ($null -eq $vrc -or ($vrc -ne 0 -and $vrc -ne 2)) {
    L ("VERIFIER RESET FAILED exit=" + $vrc); exit 1
}
$mmKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
try { $vp2 = Get-ItemProperty -LiteralPath $mmKey -ErrorAction Stop } catch { $vp2 = $null }
if ($null -eq $vp2) { L 'VERIFIER STATE READ FAILED - cannot confirm reset'; exit 1 }
$vd2 = @($vp2.VerifyDrivers | Where-Object { $null -ne $_ -and ("$_").Trim() -ne '' })
$lvl2 = $vp2.VerifyDriverLevel
$lvlOk = ($null -eq $lvl2 -or [int]$lvl2 -eq 0)
if ($vd2.Count -ne 0 -or -not $lvlOk) {
    L ("VERIFIER STATE NOT CLEARED: drivers=" + ($vd2 -join ',') + " level=" + $lvl2)
    exit 1
}
L ("[3] verifier reset OK (exit=" + $vrc + ", registry cleared; reboot required to take effect)")

# --- 4) refresh staging copy + record hashes ---
Copy-Item -LiteralPath $setup -Destination $stage -Force
$hfile = Join-Path $logDir 'batch-hashes.txt'
Add-Content -LiteralPath $hfile -Value ("FINAL_SETUP_SHA256=" + $setupHash)
Add-Content -LiteralPath $hfile -Value ("FINAL_SYS_SHA256=" + $sysHash0)

# [QA X2 fix] Reboot marker. verify_final_reboot.ps1 must prove a REAL reboot
# happened after this UAC run. Without it, "verifier /reset + no reboot" could
# still print VERDICT: PASS (registry already cleared, kernel verification still
# on, D: auto-resume never exercised). Written LAST, only after every step
# succeeded, so a half-finished UAC never leaves a valid-looking marker.
# Fixed-width ISO-8601 UTC (24 chars) -> lexicographic order == time order,
# avoiding [datetime]::TryParse overload binding quirks on Windows PS 5.1.
$marker = Join-Path $logDir 'finalize-utc.txt'
try {
    $markerText = [datetime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture)
    [IO.File]::WriteAllText($marker, $markerText, (New-Object Text.UTF8Encoding($false)))
    $back = ([IO.File]::ReadAllText($marker, (New-Object Text.UTF8Encoding($false)))).Trim()
    if ($back.Length -ne 24 -or -not $back.EndsWith('Z') -or $back -ne $markerText) {
        L ("REBOOT MARKER VERIFY FAILED: '" + $back + "'"); exit 1
    }
    L ("[4] reboot marker written utc=" + $markerText)
} catch {
    L ("REBOOT MARKER WRITE FAILED: " + $_.Exception.Message); exit 1
}

L 'ALL OK - reboot once: Verifier off + D: auto-resume + bridge with fixed modules'
Write-Host '=== finalize OK - reboot once ==='
exit 0
