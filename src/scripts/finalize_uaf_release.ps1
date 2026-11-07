# DEPRECATED 2026-11-07 -- DO NOT RUN. Use finalize_geom_rebuild.ps1 instead.
# This ONE-SHOT script pins $oldSys='ed1bd99c...' plus two stale Setup hashes;
# replaying it against the current dist (sys 63ad0fa8... / Setup 0269fb50...)
# aborts at step 1 and can leave a half-signed state. Kept ONLY as historical
# evidence of the 2026-11-06 23:29 real-hardware replay. The wrapper
# RB-finalize-uaf-release.cmd now refuses to execute outright.
# finalize_uaf_release.ps1 -- ONE UAC for the >4K geometry UAF fix FULL REPLAY.
# 2026-11-06. Log: C:\ProgramData\rb-deploy\finalize-uaf-release.log
#
# Sequence (73 #5 steps 2..8, fail-closed at EVERY step):
#   1) preconditions (fixed cache.c hash, signed baseline sys, signtool,
#      admin) + anti-reboot guard
#   2) rebuild driver (_build_manual.ps1, CHILD process)
#   3) sign new sys on ASCII stage -> dist + sha256 sidecar
#   4) sync three anchors (sync_anchors.ps1, CHILD) + read-back assertions
#   5) gate5 (check_signature.ps1, CHILD, exit code + 'result: PASS' text)
#   6) rebuild Setup (build_setup.ps1, CHILD); capture UNSIGNED baseline hash
#   7) sign Setup (four hash assertions: dist baseline / stage overwrite /
#      signed stage / copy-back)
#   8) verifier /reset (exit 0 or 2 + registry fail-closed) -> deploy.ps1
#      (CHILD) -> gate5 again
#   9) write reboot marker LAST -> ALL OK
#
# HARD RULES (73 #5.1 / 71 G1 / 71 G3 / 71 #2.2):
#   * $ErrorActionPreference = 'Continue'. NEVER 'Stop' together with native
#     stderr 2>&1 -- that is the 18:04 RB-finalize-harvest.cmd abort root cause.
#   * Every native / child invocation goes through Invoke-Step: a -999
#     $global:LASTEXITCODE sentinel is set IMMEDIATELY BEFORE the call and
#     captured into an explicit ExitCode IMMEDIATELY AFTER. No inherited value
#     can ever masquerade as success.
#   * gate5 is a CHILD PROCESS; judge BOTH exit code and 'result: PASS' text.
#   * verifier /reset: exit 0 OR 2 is success; the registry is the authority.
#   * Unsigned Setup baseline hash is computed DYNAMICALLY after step 6.
#     Hardcoded old payload constants are FORBIDDEN-RESURRECTION values only.
#   * README.txt is a sys anchor: assert that NOTHING but the hash line moved.
#   * Marker is written ONLY after every step succeeded. Re-running this
#     wrapper AFTER the reboot would overwrite it and break the
#     boot_utc > finalize_utc judgement in verify_final_reboot.ps1.
#   * ONE-SHOT: if it aborts mid-way, do NOT blindly re-run. Inspect the log:
#     dist\RAMBlockCache.sys may already be the NEW signed image, which fails
#     the step-1 baseline assertion by design.
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
$log     = Join-Path $logDir 'finalize-uaf-release.log'
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$psExe   = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$verExe  = Join-Path $env:WINDIR 'System32\verifier.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'

$cacheC      = Join-Path $root 'src\core\cache.c'
$distSys     = Join-Path $root 'dist\RAMBlockCache.sys'
$builtSys    = Join-Path $root 'src\driver\x64\Release\RAMBlockCache.sys'
$sidecar     = $distSys + '.sha256'
$setup       = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
$stageSys    = Join-Path $logDir 'RAMBlockCache.sys'
$stageSetup  = Join-Path $logDir 'RAMBlockCache-Setup.exe'
$marker      = Join-Path $logDir 'finalize-utc.txt'
$hashLedger  = Join-Path $logDir 'batch-hashes.txt'
$buildDriver = Join-Path $root 'src\driver\_build_manual.ps1'
$gate        = Join-Path $PSScriptRoot 'check_signature.ps1'
$syncAnchors = Join-Path $PSScriptRoot 'sync_anchors.ps1'
$buildSetup  = Join-Path $PSScriptRoot 'build_setup.ps1'
$deploy      = Join-Path $PSScriptRoot 'deploy.ps1'
$deployPs    = Join-Path $PSScriptRoot 'deploy.ps1'
$verifySig   = Join-Path $PSScriptRoot 'verify_deploy_signature.ps1'
$readme      = Join-Path $root 'README.txt'

# Fixed source pin (the candidate-A one-line fix, 3001 lines).
$expectCache = '8340f9ff34de49a3ad09ff3a3f62f93d53a002f60c0e3e8dadea5e51b9e7cfde'
# Old signed sys baseline; the rebuild MUST produce a different image.
$oldSys = 'ed1bd99cd56812a36c2adb2e6f60f79b87512886bd2d02214eb25840663ab68e'
# Forbidden-resurrection Setup payloads. 6acb825f... is CURRENTLY the signed
# dist Setup AND the ASCII stage payload (19:05); 7912d276... is the older
# 15:50 build from 71 #2.2. Neither may ever be signed or copied back again.
$staleSetup    = '6acb825f83b164727bc287f5fde12e15f081b4cc91565f4086379e69306d671d'
$staleSetupOld = '7912d2760efc4249aafd6269a22e2e40fa1bdda39cdd4f888bc48d63c30867af'
$forbiddenSetup = @($staleSetup, $staleSetupOld)

function L([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m)
}

function Abort([string]$m) {
    L ('ABORT: ' + $m)
    Write-Host ('[X] ' + $m)
    exit 1
}

# Single choke point for ALL native / child-process calls.
# EAP='Continue' => native stderr through 2>&1 is NON-terminating (71 G1).
# -999 sentinel immediately before + explicit ExitCode immediately after:
# an inherited value can never look green.
function Invoke-Step {
    param([string]$Tag, [string]$Exe, [string[]]$ArgList)
    L ($Tag + ' start: ' + $Exe + ' ' + ($ArgList -join ' '))
    $global:LASTEXITCODE = -999
    $o = & $Exe @ArgList 2>&1
    $rc = $LASTEXITCODE
    $o | ForEach-Object { L ($Tag + ': ' + $_) }
    L ($Tag + ' exit=' + $rc)
    $result = [pscustomobject]@{ ExitCode = $rc; Output = $o }
    return $result
}

function HashOf([string]$p) {
    try { return (Get-FileHash -LiteralPath $p -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower() }
    catch { Abort ('HASH READ FAILED: ' + $p + ' :: ' + $_.Exception.Message) }
}

function SigOf([string]$p) {
    try { return (Get-AuthenticodeSignature -LiteralPath $p -ErrorAction Stop).Status }
    catch { Abort ('SIGNATURE READ FAILED: ' + $p + ' :: ' + $_.Exception.Message) }
}

function CopyChecked([string]$src, [string]$dst, [string]$tag) {
    try { Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop }
    catch { Abort ($tag + ' COPY FAILED: ' + $_.Exception.Message) }
}

if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
if (-not (Test-Path -LiteralPath $log)) { '' | Set-Content -LiteralPath $log }
L ('=== finalize_uaf_release start pid=' + $PID + ' ===')

# ---- 1) preconditions + anti-reboot guard -------------------------------
foreach ($f in @($sigtool, $psExe, $verExe, $cacheC, $distSys, $setup,
                  $buildDriver, $gate, $syncAnchors, $buildSetup, $deploy,
                  $deployPs, $verifySig, $readme)) {
    if (-not (Test-Path -LiteralPath $f)) { Abort ('MISSING: ' + $f) }
}
Set-Location -LiteralPath $root

$cacheHash = HashOf $cacheC
if ($cacheHash -ne $expectCache) { Abort ('CACHE.C HASH MISMATCH (not the fixed source): ' + $cacheHash) }
L ('[1] cache.c fixed hash OK ' + $cacheHash)

$sysHash0 = HashOf $distSys
if ($sysHash0 -ne $oldSys) { Abort ('SYS BASELINE MISMATCH: ' + $sysHash0) }
if ((SigOf $distSys) -ne 'Valid') { Abort ('SYS BASELINE SIGNATURE NOT VALID: ' + $sysHash0) }
L ('[1] sys baseline OK ' + $sysHash0 + ' status=Valid')

$setupHash0 = HashOf $setup
$sSetup0 = SigOf $setup
L ('[1] setup before sha256=' + $setupHash0 + ' status=' + $sSetup0 + ' size=' + (Get-Item -LiteralPath $setup).Length)

# A marker newer than the last boot means a previous UAC finished but the
# machine has NOT been rebooted since it. Re-running now would overwrite the
# marker and break boot_utc > finalize_utc in verify_final_reboot.ps1.
if (Test-Path -LiteralPath $marker) {
    try {
        $mText = ([IO.File]::ReadAllText($marker, (New-Object Text.UTF8Encoding($false)))).Trim()
        $bootUtc = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture)
    } catch { Abort ('MARKER / BOOT TIME READ FAILED: ' + $_.Exception.Message) }
    if ($mText.Length -ne 24 -or -not $mText.EndsWith('Z')) { Abort 'MARKER MALFORMED (expected 24-char ISO-8601 UTC)' }
    if ($bootUtc -le $mText) {
        L 'REFUSE: finalize-utc.txt exists and NO reboot happened since it.'
        L 'Re-running now would overwrite the marker and break boot_utc > finalize_utc.'
        L 'Reboot ONCE first. Then use verify_final_reboot.ps1 (read-only) to close the'
        L 'previous cycle; re-run THIS wrapper only if a NEW full replay is intended'
        L '(that run will again require exactly one further reboot).'
        Abort 'refusing to re-run before the pending reboot'
    }
    L ('[1] pre-existing marker ' + $mText + ' predates last boot ' + $bootUtc + '; marker will be refreshed ONLY after all steps pass')
}

# ---- 2) rebuild driver (CHILD) -----------------------------------------
$buildStart = [datetime]::UtcNow
$r2 = Invoke-Step -Tag '[2] driver build' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$buildDriver)
if ($r2.ExitCode -ne 0) { Abort ('DRIVER BUILD FAILED exit=' + $r2.ExitCode) }
if (-not (Test-Path -LiteralPath $builtSys)) { Abort ('BUILT SYS MISSING: ' + $builtSys) }
$builtHash = HashOf $builtSys
if ($builtHash -eq $oldSys) { Abort 'BUILT SYS HASH == OLD SIGNED SYS (build did not produce a new binary)' }
if ((Get-Item -LiteralPath $builtSys).LastWriteTimeUtc -lt $buildStart.AddSeconds(-30)) { Abort 'BUILT SYS IS NOT FRESH (timestamp predates this build)' }
if ((SigOf $builtSys) -ne 'NotSigned') { Abort 'BUILT SYS IS NOT UNSIGNED (refusing to sign a stale/pre-signed image)' }
L ('[2] rebuilt sys OK unsigned sha256=' + $builtHash)

# ---- 3) sign new sys ----------------------------------------------------
CopyChecked $builtSys $stageSys '[3] sys stage'
$stageSysHash0 = HashOf $stageSys
if ($stageSysHash0 -ne $builtHash) { Abort ('SYS STAGE OVERWRITE MISMATCH: ' + $stageSysHash0) }
L ('[3] sys stage overwritten OK sha256=' + $stageSysHash0)

$r3 = Invoke-Step -Tag '[3] signtool sys' -Exe $sigtool -ArgList @('sign','/sm','/s','My','/sha1',$thumb,'/fd','SHA256',$stageSys)
if ($r3.ExitCode -ne 0) { Abort ('SYS SIGN FAILED exit=' + $r3.ExitCode) }
if ((SigOf $stageSys) -ne 'Valid') { Abort 'SYS SIGNATURE ON STAGE NOT VALID' }
$sysHash = HashOf $stageSys
if ($sysHash -eq $oldSys) { Abort 'SIGNED SYS HASH == OLD SYS HASH' }
L ('[3] sys signed OK sha256=' + $sysHash)

CopyChecked $stageSys $distSys '[3] sys copy-back'
$distBack = HashOf $distSys
if ($distBack -ne $sysHash) { Abort ('SYS COPY-BACK MISMATCH: ' + $distBack) }
if ((SigOf $distSys) -ne 'Valid') { Abort 'DIST SYS NOT VALID AFTER COPY-BACK' }
try { ('SHA256: ' + $sysHash) | Set-Content -LiteralPath $sidecar -Encoding ASCII -ErrorAction Stop }
catch { Abort ('SIDECAR WRITE FAILED: ' + $_.Exception.Message) }
try { $sideText = ([IO.File]::ReadAllText($sidecar)).Trim() }
catch { Abort ('SIDECAR READ FAILED: ' + $_.Exception.Message) }
if ($sideText -ne ('SHA256: ' + $sysHash)) { Abort ('SIDECAR READ-BACK MISMATCH: ' + $sideText) }
L ('[3] dist sys + sidecar OK ' + $sysHash)

# ---- 4) sync three anchors (CHILD) + read-back assertions ---------------
$rxAnchor = [regex]'DefaultExpectedSha256\s*=\s*''?([0-9a-fA-F]{64})''?'
$rxNorm   = [regex]'DefaultExpectedSha256\s*=\s*''?[0-9a-fA-F]{64}''?'
try {
    $readmeBefore = [IO.File]::ReadAllText($readme)
    $readmeBeforeLen = (Get-Item -LiteralPath $readme).Length
} catch { Abort ('README PRE-READ FAILED: ' + $_.Exception.Message) }
$readmeBeforeNorm = $rxNorm.Replace($readmeBefore, 'DefaultExpectedSha256 = <HASH>')

$r4 = Invoke-Step -Tag '[4] sync_anchors' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$syncAnchors)
if ($r4.ExitCode -ne 0) { Abort ('ANCHOR SYNC FAILED exit=' + $r4.ExitCode) }

foreach ($f in @($deployPs, $verifySig, $readme)) {
    try { $t = [IO.File]::ReadAllText($f) } catch { Abort ('ANCHOR READ FAILED: ' + $f) }
    $m = $rxAnchor.Match($t)
    if (-not $m.Success -or $m.Groups[1].Value.ToLower() -ne $sysHash) { Abort ('ANCHOR READ-BACK MISMATCH: ' + $f) }
}
try {
    $readmeAfter = [IO.File]::ReadAllText($readme)
    $readmeAfterLen = (Get-Item -LiteralPath $readme).Length
} catch { Abort ('README POST-READ FAILED: ' + $_.Exception.Message) }
if ($rxNorm.Replace($readmeAfter, 'DefaultExpectedSha256 = <HASH>') -ne $readmeBeforeNorm) { Abort 'README.txt CHANGED BEYOND THE SYS HASH LINE' }
if ($readmeAfterLen -ne $readmeBeforeLen) { Abort 'README.txt BYTE LENGTH CHANGED (both hashes are 64 chars; expected identical length)' }
L ('[4] three anchors + README.txt-only-hash-line OK ' + $sysHash)

# ---- 5) gate5 (CHILD, exit code + result text) --------------------------
$g5 = Invoke-Step -Tag '[5] gate5' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$gate)
$gateTextOk = [bool]($g5.Output -match 'result: PASS')
if ($g5.ExitCode -ne 0 -or -not $gateTextOk) { Abort ('GATE5 FAILED exit=' + $g5.ExitCode + ' resultText=' + $gateTextOk) }
L ('[5] gate5 PASS exit=' + $g5.ExitCode)

# ---- 6) rebuild Setup (CHILD), capture DYNAMIC unsigned baseline --------
$setupStart = [datetime]::UtcNow
$r6 = Invoke-Step -Tag '[6] build_setup' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$buildSetup)
if ($r6.ExitCode -ne 0) { Abort ('SETUP REBUILD FAILED exit=' + $r6.ExitCode) }
if (-not (Test-Path -LiteralPath $setup)) { Abort 'SETUP MISSING AFTER REBUILD' }
$expectSetup = HashOf $setup
if ($expectSetup -eq $staleSetup -or $expectSetup -eq $staleSetupOld) { Abort 'REBUILT SETUP IS A FORBIDDEN STALE IMAGE' }
if ($expectSetup -eq $setupHash0) { Abort 'REBUILT SETUP IS UNCHANGED FROM THE PRE-REBUILD SIGNED BASELINE' }
if ((SigOf $setup) -ne 'NotSigned') { Abort 'REBUILT SETUP IS NOT UNSIGNED' }
if ((Get-Item -LiteralPath $setup).LastWriteTimeUtc -lt $setupStart.AddSeconds(-30)) { Abort 'REBUILT SETUP IS NOT FRESH' }
L ('[6] unsigned Setup baseline (dynamic) sha256=' + $expectSetup + ' size=' + (Get-Item -LiteralPath $setup).Length)

# ---- 7) sign Setup: FOUR hash assertions --------------------------------
# (a) dist unsigned baseline
if ($expectSetup -in $forbiddenSetup) { Abort 'UNSIGNED SETUP BASELINE IS A FORBIDDEN STALE HASH' }
if ((SigOf $setup) -ne 'NotSigned') { Abort 'SETUP BASELINE IS NOT UNSIGNED' }
# (b) stage overwrite: prove the ASCII stage was really replaced
CopyChecked $setup $stageSetup '[7] setup stage'
$stageHash0 = HashOf $stageSetup
if ($stageHash0 -ne $expectSetup) { Abort ('SETUP STAGE OVERWRITE MISMATCH: ' + $stageHash0) }
if ($stageHash0 -in $forbiddenSetup) { Abort 'SETUP STAGE STILL HOLDS A FORBIDDEN STALE IMAGE' }
L ('[7] (a)(b) dist baseline + stage overwrite OK ' + $stageHash0)
# (c) sign + signed stage hash
$r7 = Invoke-Step -Tag '[7] signtool setup' -Exe $sigtool -ArgList @('sign','/sm','/s','My','/sha1',$thumb,'/fd','SHA256',$stageSetup)
if ($r7.ExitCode -ne 0) { Abort ('SETUP SIGN FAILED exit=' + $r7.ExitCode) }
if ((SigOf $stageSetup) -ne 'Valid') { Abort 'SETUP SIGNATURE ON STAGE NOT VALID' }
$signedStageHash = HashOf $stageSetup
if ($signedStageHash -eq $expectSetup -or $signedStageHash -in $forbiddenSetup) { Abort 'SIGNED STAGE HASH INVALID (unchanged or stale)' }
L ('[7] (c) signed stage sha256=' + $signedStageHash)
# (d) copy back + dist consistency + final Valid
CopyChecked $stageSetup $setup '[7] setup copy-back'
$copyBackHash = HashOf $setup
if ($copyBackHash -ne $signedStageHash) { Abort ('SETUP COPY-BACK MISMATCH: ' + $copyBackHash) }
if ((SigOf $setup) -ne 'Valid') { Abort 'DIST SETUP NOT VALID AFTER COPY-BACK' }
if ($copyBackHash -eq $staleSetup -or $copyBackHash -eq $staleSetupOld) { Abort 'DIST SETUP IS A FORBIDDEN STALE IMAGE' }
L ('[7] (d) final signed Setup sha256=' + $copyBackHash + ' size=' + (Get-Item -LiteralPath $setup).Length)

# ---- 8) verifier /reset + deploy (CHILD) + gate5 again ------------------
$r8 = Invoke-Step -Tag '[8] verifier' -Exe $verExe -ArgList @('/reset')
if ($null -eq $r8.ExitCode -or ($r8.ExitCode -ne 0 -and $r8.ExitCode -ne 2)) { Abort ('VERIFIER RESET FAILED exit=' + $r8.ExitCode) }
$mmKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
try { $vp2 = Get-ItemProperty -LiteralPath $mmKey -ErrorAction Stop } catch { $vp2 = $null }
if ($null -eq $vp2) { Abort 'VERIFIER STATE READ FAILED - cannot confirm reset' }
$vd2 = @($vp2.VerifyDrivers | Where-Object { $null -ne $_ -and ("$_").Trim() -ne '' })
$lvl2 = $vp2.VerifyDriverLevel
$lvlOk = $false
if ($null -eq $lvl2) { $lvlOk = $true }
else { try { $lvlOk = ([int]$lvl2 -eq 0) } catch { $lvlOk = $false } }
if ($vd2.Count -ne 0 -or -not $lvlOk) { Abort ('VERIFIER STATE NOT CLEARED drivers=' + ($vd2 -join ',') + ' level=' + $lvl2) }
L ('[8] verifier reset OK exit=' + $r8.ExitCode + ' registry cleared (reboot required)')

$r8b = Invoke-Step -Tag '[8] deploy' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$deploy)
if ($r8b.ExitCode -ne 0) { Abort ('DEPLOY FAILED exit=' + $r8b.ExitCode) }
L '[8] deploy OK (no reboot triggered)'

$g8 = Invoke-Step -Tag '[8] gate5 post-deploy' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$gate)
$g8TextOk = [bool]($g8.Output -match 'result: PASS')
if ($g8.ExitCode -ne 0 -or -not $g8TextOk) { Abort ('POST-DEPLOY GATE5 FAILED exit=' + $g8.ExitCode) }
L '[8] post-deploy gate5 PASS (three anchors still consistent)'

# ---- 9) ledger (best effort) then marker LAST ---------------------------
try {
    Add-Content -LiteralPath $hashLedger -Value ('FINAL_SETUP_SHA256=' + $copyBackHash) -ErrorAction Stop
    Add-Content -LiteralPath $hashLedger -Value ('FINAL_SYS_SHA256=' + $sysHash) -ErrorAction Stop
    L ('[9] hash ledger appended ' + $hashLedger)
} catch {
    L ('[warn] hash ledger append failed (non-fatal; dist remains authoritative): ' + $_.Exception.Message)
}

try {
    $markerText = [datetime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture)
    [IO.File]::WriteAllText($marker, $markerText, (New-Object Text.UTF8Encoding($false)))
    $back = ([IO.File]::ReadAllText($marker, (New-Object Text.UTF8Encoding($false)))).Trim()
} catch { Abort ('REBOOT MARKER WRITE/READ FAILED: ' + $_.Exception.Message) }
if ($back.Length -ne 24 -or -not $back.EndsWith('Z') -or $back -ne $markerText) { Abort ('REBOOT MARKER VERIFY FAILED: ' + $back) }
L ('[9] reboot marker written utc=' + $markerText)

L ('ALL OK sys=' + $sysHash + ' setup=' + $copyBackHash)
L 'Reboot ONCE, wait 60-90s, then run verify_final_reboot.ps1 (read-only).'
L 'DO NOT re-run this wrapper after the reboot.'
Write-Host '=== finalize_uaf_release OK - reboot once ==='
exit 0
