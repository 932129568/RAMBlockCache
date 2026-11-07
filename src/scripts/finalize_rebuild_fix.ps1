# finalize_rebuild_fix.ps1 -- ONE UAC for the [92 A/B/C] rebuild-drain fix.
# 2026-11-07. Log: C:\ProgramData\rb-deploy\finalize-rebuild-fix.log
#
# Why this exists:
#   The volume-level geometry rebuild timed out deterministically on real
#   hardware (91/92: ERROR [453] detach drain timeout, [268] datapath quiesce
#   timeout, [334] stream(s) still attached). The fix touches rbattach.c /
#   rbrebuild.c / rb_common.h, so the driver must be rebuilt and re-signed.
#   A full-chain replay would overwrite the reboot marker and run unrelated
#   test-flag reset steps. THIS chain is intentionally narrow:
#   sign the already-built driver, sync the three anchors, gate5, repack and
#   re-sign the Setup, deploy.ps1 (CHILD), post-deploy gate5, ledger.
#   [92 3.1 ERRATA] deploy.ps1 is MANDATORY: it is the only step that copies
#   dist\RAMBlockCache.sys into System32\drivers. Signing dist without
#   deploying means the rebooted kernel still loads the OLD sys and the
#   whole fix is a no-op. NO test-flag reset, NO reboot, NO SET_CONFIG.
#   [94 5.2 / 96 review] the reboot marker IS written - but ONLY LAST,
#   after every check passed (step 11), so X2 boot>marker genuinely proves
#   this UAC rebooted.
#
# Derived from finalize_setup_only.ps1 / finalize_geom_rebuild.ps1.
# Every hard discipline is kept:
#   * $ErrorActionPreference = 'Continue' (NEVER 'Stop' with native 2>&1).
#   * Invoke-Step: -999 sentinel IMMEDIATELY BEFORE + explicit ExitCode
#     IMMEDIATELY AFTER every native / child invocation.
#   * HashOf / SigOf / CopyChecked; independent log; Write-Host is display-only.
#   * gate5 is a CHILD process judged by BOTH exit code and 'result: PASS' text.
#   * marker discipline: finalize-utc.txt is byte-compared at step 10, then
#     written ONLY at step 11 (LAST, post-verification; 94 5.2).
#   * scope double-read: sys / sidecar / three anchors / marker at start and end.
#
# Sequence:
#   1) admin + preconditions (built sys NotSigned, old dist sys Valid,
#      three anchors agree, gate5 PASS, marker read) + scope statement
#   2) sign new sys on the ASCII stage -> Valid -> dist + sidecar
#   3) sync three anchors (CHILD) + read-back (README.txt hash line ONLY)
#   4) gate5 again (CHILD, exit code + result text)
#   5) build_setup.ps1 (CHILD) -> fresh unsigned Setup
#   6) payload assertions (CLI pair, both zips, bridge decoder, cwd-independent
#      bridge index.ts, five-size SPA, new sys in payload)
#   7) sign Setup: four hash assertions
#   8) deploy.ps1 (CHILD, -999 sentinel + explicit ExitCode) -> post-deploy
#      gate5 (CHILD, exit code + 'result: PASS' text double judgement)
#   9) FATAL ledger append FINAL_SETUP_SHA256 + own read-back
#  10) final scope double-read
#  11) reboot marker written LAST (94 5.2)
#  12) ALL OK + NEXT hint
#
# DO NOT DEPLOY: 31cf870 / 76aacfc / 931ed53 are AUDIT RECORDS ONLY
# (93 SPEC NO-GO, see the fail-closed gate below and the 93 report).
#
# NEXT (plain session, by the lead): sync_setup_hash.ps1 -RequireSetupChanged,
# then ONE reboot AND AN INTERACTIVE LOGON (logon_disarm must run to clear
# FilterArmed, otherwise verify_final_reboot.ps1 gives a deterministic false
# FAIL), then verify_final_reboot.ps1 and the S4 geometry matrix.
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
$log     = Join-Path $logDir 'finalize-rebuild-fix.log'
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$psExe   = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$tarExe  = Join-Path $env:WINDIR 'System32\tar.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'

$builtSys    = Join-Path $root 'src\driver\x64\Release\RAMBlockCache.sys'
$distSys     = Join-Path $root 'dist\RAMBlockCache.sys'
$sidecar     = $distSys + '.sha256'
$stageSys    = Join-Path $logDir 'RAMBlockCache.sys'
$setup       = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
$stageSetup  = Join-Path $logDir 'RAMBlockCache-Setup.exe'
$marker      = Join-Path $logDir 'finalize-utc.txt'
$hashLedger  = Join-Path $logDir 'batch-hashes.txt'
$gate        = Join-Path $PSScriptRoot 'check_signature.ps1'
$syncAnchors = Join-Path $PSScriptRoot 'sync_anchors.ps1'
$buildSetup  = Join-Path $PSScriptRoot 'build_setup.ps1'
$deploy      = Join-Path $PSScriptRoot 'deploy.ps1'   # [92 3.1] MANDATORY child
$readme      = Join-Path $root 'README.txt'
$ctlExe      = Join-Path $root 'src\ctl\ramboostctl.exe'
$ctlFeatExe  = Join-Path $root 'src\ctl\rbfeatctl.exe'
$pay         = Join-Path $root 'src\ui\installer\RAMBlockCache.Setup\Assets\payload'
$payloadCtl  = Join-Path $pay 'ramboostctl.exe'
$payloadFeat = Join-Path $pay 'rbfeatctl.exe'
$payloadSys  = Join-Path $pay 'RAMBlockCache.sys'
$wcZip       = Join-Path $pay 'webconsole.zip'
$ctlZip      = Join-Path $pay 'controller.zip'

# The three sys-hash anchors. Anchor A is the deployment tool itself, which
# carries the $DefaultExpectedSha256 pin. [92 3.1] That tool is ALSO invoked
# as a CHILD at step 8 (mandatory, see the errata); it is an anchor READ here,
# not a deployment call. It does not write the reboot marker (marker is owned
# by the full-chain wrapper, which this chain never runs) and does not touch
# bootcfg (AutoAttach / ResumeOnBoot / ResumeLetters use a separate ABI).
$anchorA     = $deploy
$anchorB     = Join-Path $PSScriptRoot 'verify_deploy_signature.ps1'
$anchorC     = $readme
$anchorFiles = @($anchorA, $anchorB, $anchorC)
$rxAnchor    = [regex]'DefaultExpectedSha256\s*=\s*''?([0-9a-fA-F]{64})''?'
$rxNorm      = [regex]'DefaultExpectedSha256\s*=\s*''?[0-9a-fA-F]{64}''?'

$statsFields  = @('EngineBlockSize','GeometryRebuildCount','GeometryRebuildLastStatus','GeometryRebuildInProgress','LastGeometryRebuildUtc')
$statvalKeys  = @('engine_bs','rebuild_count','rebuild_status','rebuild_in_progress','rebuild_utc')
$blockLabels  = @('4 KB','8 KB','16 KB','32 KB','64 KB')
$rxBlockArray = [regex]'\[\s*4096\s*,\s*8192\s*,\s*16384\s*,\s*32768\s*,\s*65536\s*\]'
$rxChunk      = [regex]'SettingsPage-[A-Za-z0-9_-]+\.js$'

# ---- [93 SPEC NO-GO] FAIL-CLOSED FREEZE GATE -----------------------------
# DO NOT DEPLOY: commits 31cf870 / 76aacfc / 931ed53 are AUDIT RECORDS ONLY.
# Independent second-layer review deliverables/93-spec-nogo-freeze-2026-11-07.md
# proved the "Refs relative baseline + 4 x 20ms stable" drain criterion is
# FAIL-OPEN: a pre-window IO whose +1 is baked into the baseline and that lives
# >80ms is judged "drained" -> engine destroyed -> UAF / silent data corruption
# on a BOOT_START volume filter. PE can rescue boot, NOT user data.
# NO automatic / date-based / environment-variable bypass. The lift file below
# must be created MANUALLY by the lead only after route A is re-designed AND
# independently reviewed SPEC OK.
# This gate runs after the variable area and BEFORE the first write operation
# (log init / Copy-Item / Set-Content / Invoke-Step), so a frozen chain cannot
# sign, deploy or reboot anything.
$nogoLift = 'C:\ProgramData\rb-deploy\94-SPEC-OK-LIFTED.ok'
if (-not (Test-Path -LiteralPath $nogoLift)) {
    Write-Host '[X] FROZEN: 94-SPEC-OK-LIFTED.ok missing - DO NOT DEPLOY.'
    Write-Host '    History: 31cf870/76aacfc/931ed53 were frozen by 93 SPEC NO-GO (fail-open UAF).'
    Write-Host '    The 94-spec implementation (a648932/fb8a54f) passed independent review'
    Write-Host '    SPEC OK (deliverables/qa-rebuild-spec-96-review-2026-11-07.md), but the'
    Write-Host '    deploy gate stays fail-closed until the lead MANUALLY creates the lift file.'
    Write-Host ('    Manual lift file required (lead only): ' + $nogoLift)
    exit 1
}
Write-Host ('[i] 94 SPEC OK lift present: ' + $nogoLift + ' - continuing')

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
# -999 sentinel immediately before + explicit ExitCode immediately after.
# -Quiet only suppresses dumping large payload text into the log; the sentinel,
# the explicit ExitCode and the full Output capture are unchanged.
function Invoke-Step {
    param([string]$Tag, [string]$Exe, [string[]]$ArgList, [switch]$Quiet)
    L ($Tag + ' start: ' + $Exe + ' ' + ($ArgList -join ' '))
    $global:LASTEXITCODE = -999
    $o = & $Exe @ArgList 2>&1
    $rc = $LASTEXITCODE
    if ($Quiet) {
        L ($Tag + ': output lines=' + @($o).Count)
    } else {
        $o | ForEach-Object { L ($Tag + ': ' + $_) }
    }
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

function Get-AnchorState([string]$Tag) {
    $vals = New-Object 'System.Collections.Generic.List[string]'
    foreach ($f in $anchorFiles) {
        try { $t = [IO.File]::ReadAllText($f) }
        catch { Abort ($Tag + ' ANCHOR READ FAILED: ' + $f) }
        $m = $rxAnchor.Match($t)
        if (-not $m.Success) { Abort ($Tag + ' ANCHOR PARSE FAILED: ' + $f) }
        $vals.Add($m.Groups[1].Value.ToLower())
    }
    return ,$vals.ToArray()
}

function Invoke-Gate5([string]$Tag) {
    $g = Invoke-Step -Tag $Tag -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$gate)
    $textOk = [bool]($g.Output -match 'result: PASS')
    if ($g.ExitCode -ne 0 -or -not $textOk) {
        Abort ($Tag + ' FAILED exit=' + $g.ExitCode + ' resultText=' + $textOk)
    }
    L ($Tag + ' PASS exit=' + $g.ExitCode + ' resultText=' + $textOk)
}

if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
if (-not (Test-Path -LiteralPath $log)) { '' | Set-Content -LiteralPath $log }
L ('=== finalize_rebuild_fix start pid=' + $PID + ' ===')

# ---- 1) preconditions + dynamic baselines + scope statement --------------
$required = @($sigtool, $psExe, $tarExe, $builtSys, $distSys, $sidecar,
              $setup, $marker, $hashLedger, $gate, $syncAnchors, $buildSetup, $deploy,
              $ctlExe, $ctlFeatExe, $payloadCtl, $payloadFeat,
              $payloadSys, $wcZip, $ctlZip) + $anchorFiles
foreach ($f in $required) {
    if (-not (Test-Path -LiteralPath $f)) { Abort ('MISSING: ' + $f) }
}
Set-Location -LiteralPath $root

$builtHash = HashOf $builtSys
if ((SigOf $builtSys) -ne 'NotSigned') { Abort ('BUILT SYS MUST BE UNSIGNED: ' + $builtHash) }
$oldSys = HashOf $distSys
if ($builtHash -eq $oldSys) { Abort 'BUILT SYS HASH == OLD SIGNED SYS (driver was not rebuilt for 92 A/B/C)' }
if ((SigOf $distSys) -ne 'Valid') { Abort ('OLD DIST SYS NOT VALID: ' + $oldSys) }
try { $sideText0 = ([IO.File]::ReadAllText($sidecar)).Trim() }
catch { Abort ('SYS SIDECAR READ FAILED: ' + $_.Exception.Message) }
if ($sideText0 -ne ('SHA256: ' + $oldSys)) { Abort ('OLD SYS SIDECAR MISMATCH: ' + $sideText0) }

$a0 = Get-AnchorState '[1]'
foreach ($v in $a0) {
    if ($v -ne $oldSys) { Abort ('SYS ANCHOR MISMATCH: ' + $v + ' != ' + $oldSys) }
}
L ('[1] old sys ' + $oldSys + ' Valid + sidecar + three anchors agree')

$oldSetup = HashOf $setup
if ((SigOf $setup) -ne 'Valid') { Abort ('OLD SETUP NOT VALID: ' + $oldSetup) }
try {
    $markerText0 = ([IO.File]::ReadAllText($marker, (New-Object Text.UTF8Encoding($false)))).Trim()
} catch { Abort ('MARKER READ FAILED: ' + $_.Exception.Message) }
if ($markerText0.Length -ne 24 -or -not $markerText0.EndsWith('Z')) {
    Abort ('MARKER MALFORMED: ' + $markerText0)
}
try {
    $readmeBefore = [IO.File]::ReadAllText($readme)
    $readmeBeforeLen = (Get-Item -LiteralPath $readme).Length
} catch { Abort ('README PRE-READ FAILED: ' + $_.Exception.Message) }
$readmeBeforeNorm = $rxNorm.Replace($readmeBefore, 'DefaultExpectedSha256 = <HASH>')

Invoke-Gate5 '[1] gate5 precheck'
L ('[1] built sys ' + $builtHash + ' NotSigned; old Setup ' + $oldSetup + ' Valid; marker ' + $markerText0 + ' (READ ONLY)')

$scope = 'SCOPE: this run re-signs the driver + Setup, syncs the three sys anchors, DEPLOYS the new signed image to System32\drivers, and re-arms the watchdog (DeploySeq+1 / FilterArmed=1 / LastBootHealthy=0 / BootFails=0 + two guard tasks). deployed sys updated to the new signed image; [94 5.2] the reboot marker is written LAST (only after every check passed) so X2 boot_utc > marker genuinely proves this UAC rebooted; config / bootcfg / Features Flags unchanged (Flags already 0). No reboot, no SET_CONFIG, no test-flag reset, no verifier /reset. After reboot an INTERACTIVE LOGON is REQUIRED (logon_disarm clears FilterArmed) or verify_final_reboot.ps1 false-FAILs.'
L $scope
Write-Host ('[i] ' + $scope)

# ---- 2) sign new sys on the ASCII stage ----------------------------------
CopyChecked $builtSys $stageSys '[2] sys stage'
$stageSysHash0 = HashOf $stageSys
if ($stageSysHash0 -ne $builtHash) { Abort ('SYS STAGE OVERWRITE MISMATCH: ' + $stageSysHash0) }
L ('[2] (a)(b) built baseline + ASCII stage overwrite OK ' + $stageSysHash0)

$r2 = Invoke-Step -Tag '[2] signtool sys' -Exe $sigtool -ArgList @('sign','/sm','/s','My','/sha1',$thumb,'/fd','SHA256',$stageSys)
if ($r2.ExitCode -ne 0) { Abort ('SYS SIGN FAILED exit=' + $r2.ExitCode) }
if ((SigOf $stageSys) -ne 'Valid') { Abort 'SYS SIGNATURE ON STAGE NOT VALID' }
$sysHash = HashOf $stageSys
if ($sysHash -eq $builtHash -or $sysHash -eq $oldSys) { Abort 'SIGNED SYS HASH INVALID (unchanged or old)' }
L ('[2] (c) signed sys stage sha256=' + $sysHash)

CopyChecked $stageSys $distSys '[2] sys copy-back'
if ((HashOf $distSys) -ne $sysHash) { Abort 'SYS COPY-BACK MISMATCH' }
if ((SigOf $distSys) -ne 'Valid') { Abort 'DIST SYS NOT VALID AFTER COPY-BACK' }
try { ('SHA256: ' + $sysHash) | Set-Content -LiteralPath $sidecar -Encoding ASCII -ErrorAction Stop }
catch { Abort ('SIDECAR WRITE FAILED: ' + $_.Exception.Message) }
try { $sideText1 = ([IO.File]::ReadAllText($sidecar)).Trim() }
catch { Abort ('SIDECAR READ-BACK FAILED: ' + $_.Exception.Message) }
if ($sideText1 -ne ('SHA256: ' + $sysHash)) { Abort ('SIDECAR READ-BACK MISMATCH: ' + $sideText1) }
L ('[2] (d) dist sys + sidecar OK ' + $sysHash)

# ---- 3) sync three anchors (CHILD) + read-back ---------------------------
$r3 = Invoke-Step -Tag '[3] sync_anchors' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$syncAnchors)
if ($r3.ExitCode -ne 0) { Abort ('ANCHOR SYNC FAILED exit=' + $r3.ExitCode) }
foreach ($f in $anchorFiles) {
    try { $t = [IO.File]::ReadAllText($f) } catch { Abort ('ANCHOR READ FAILED: ' + $f) }
    $m = $rxAnchor.Match($t)
    if (-not $m.Success -or $m.Groups[1].Value.ToLower() -ne $sysHash) {
        Abort ('ANCHOR READ-BACK MISMATCH: ' + $f)
    }
}
try {
    $readmeAfter = [IO.File]::ReadAllText($readme)
    $readmeAfterLen = (Get-Item -LiteralPath $readme).Length
} catch { Abort ('README POST-READ FAILED: ' + $_.Exception.Message) }
if ($rxNorm.Replace($readmeAfter, 'DefaultExpectedSha256 = <HASH>') -ne $readmeBeforeNorm) {
    Abort 'README.txt CHANGED BEYOND THE SYS HASH LINE'
}
if ($readmeAfterLen -ne $readmeBeforeLen) {
    Abort 'README.txt BYTE LENGTH CHANGED (both hashes are 64 chars)'
}
L ('[3] three anchors + README.txt-only-hash-line OK ' + $sysHash)

# ---- 4) gate5 after anchor sync ------------------------------------------
Invoke-Gate5 '[4] gate5 post-sync'

# ---- 5) rebuild Setup (CHILD) --------------------------------------------
$setupStart = [datetime]::UtcNow
$r5 = Invoke-Step -Tag '[5] build_setup' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$buildSetup)
if ($r5.ExitCode -ne 0) { Abort ('SETUP REBUILD FAILED exit=' + $r5.ExitCode) }
if (-not (Test-Path -LiteralPath $setup)) { Abort 'SETUP MISSING AFTER REBUILD' }
$expectSetup = HashOf $setup
if ($expectSetup -eq $oldSetup) { Abort 'REBUILT SETUP IS UNCHANGED FROM THE OLD SIGNED BASELINE' }
if ((SigOf $setup) -ne 'NotSigned') { Abort 'REBUILT SETUP IS NOT UNSIGNED' }
if ((Get-Item -LiteralPath $setup).LastWriteTimeUtc -lt $setupStart.AddSeconds(-30)) {
    Abort 'REBUILT SETUP IS NOT FRESH'
}
L ('[5] unsigned Setup baseline sha256=' + $expectSetup + ' size=' + (Get-Item -LiteralPath $setup).Length)

# ---- 6) payload assertions (6b / 6c / 6d + 92 proofs) -------------------
if (-not (Test-Path -LiteralPath $payloadCtl)) { Abort ('PAYLOAD CLI MISSING: ' + $payloadCtl) }
if ((HashOf $payloadCtl) -ne (HashOf $ctlExe)) { Abort 'PAYLOAD ramboostctl.exe != src\ctl\ramboostctl.exe' }
$payloadText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($payloadCtl))
foreach ($k in $statvalKeys) {
    if (-not $payloadText.Contains($k)) { Abort ('PAYLOAD CLI MISSING statval key ' + $k) }
}
L ('[6] payload CLI matches fresh src CLI ' + (HashOf $ctlExe) + ' + 5 statval keys')

if (-not (Test-Path -LiteralPath $payloadFeat)) { Abort ('PAYLOAD FEAT CLI MISSING: ' + $payloadFeat) }
if ((HashOf $payloadFeat) -ne (HashOf $ctlFeatExe)) { Abort 'PAYLOAD rbfeatctl.exe != src\ctl\rbfeatctl.exe' }
foreach ($z in @($wcZip, $ctlZip)) {
    if (-not (Test-Path -LiteralPath $z)) { Abort ('PAYLOAD ZIP MISSING: ' + $z) }
    if ((Get-Item -LiteralPath $z).LastWriteTimeUtc -lt $setupStart.AddSeconds(-30)) {
        Abort ('PAYLOAD ZIP NOT FRESH: ' + $z)
    }
}
L '[6] payload rbfeatctl.exe + both zips refreshed OK'

# The newly signed sys MUST be the one embedded in the distribution payload.
if ((HashOf $payloadSys) -ne $sysHash) { Abort 'PAYLOAD RAMBlockCache.sys != newly signed dist sys' }
L ('[6] payload sys == newly signed sys ' + $sysHash)

$rDec = Invoke-Step -Tag '[6] webconsole decoder probe' -Exe $tarExe -ArgList @('-xOf', $wcZip, 'bridge/server/proto-decode.ts')
if ($rDec.ExitCode -ne 0) { Abort ('WEBCONSOLE DECODER PROBE FAILED exit=' + $rDec.ExitCode) }
$decText = ($rDec.Output | ForEach-Object { [string]$_ }) -join "`n"
foreach ($k in $statsFields) {
    if (-not $decText.Contains($k)) { Abort ('WEBCONSOLE ZIP STALE: proto-decode.ts missing ' + $k) }
}
L ('[6] webconsole.zip decoder carries all 5 RB_STATS fields (' + $decText.Length + ' chars probed)')

$rIdx = Invoke-Step -Tag '[6] webconsole bridge index probe' -Exe $tarExe -ArgList @('-xOf', $wcZip, 'bridge/server/index.ts') -Quiet
if ($rIdx.ExitCode -ne 0) { Abort ('WEBCONSOLE BRIDGE PROBE FAILED exit=' + $rIdx.ExitCode) }
$idxText = ($rIdx.Output | ForEach-Object { [string]$_ }) -join "`n"
if ($idxText.Contains('process.cwd()')) { Abort 'WEBCONSOLE ZIP STALE: index.ts still uses process.cwd()' }
if (-not $idxText.Contains('import.meta.url')) { Abort 'WEBCONSOLE ZIP STALE: index.ts missing import.meta.url' }
if ($idxText.Contains([string][char]0)) { Abort 'WEBCONSOLE ZIP STALE: index.ts contains a literal NUL byte' }
L ('[6] webconsole.zip index.ts is cwd-independent + NUL-free (' + $idxText.Length + ' chars probed)')

$rList = Invoke-Step -Tag '[6] webconsole listing' -Exe $tarExe -ArgList @('-tf', $wcZip) -Quiet
if ($rList.ExitCode -ne 0) { Abort ('WEBCONSOLE LIST FAILED exit=' + $rList.ExitCode) }
$entries = @($rList.Output | ForEach-Object { ([string]$_).Trim() })
$chunkEntries = @($entries | Where-Object { $rxChunk.IsMatch($_) })
if ($chunkEntries.Count -ne 1) {
    Abort ('WEBCONSOLE ZIP: expected exactly 1 SettingsPage chunk, found ' + $chunkEntries.Count)
}
$chunkName = $chunkEntries[0]
$rChunk = Invoke-Step -Tag '[6] webconsole SettingsPage chunk' -Exe $tarExe -ArgList @('-xOf', $wcZip, $chunkName) -Quiet
if ($rChunk.ExitCode -ne 0) { Abort ('WEBCONSOLE CHUNK READ FAILED exit=' + $rChunk.ExitCode) }
$chunkText = ($rChunk.Output | ForEach-Object { [string]$_ }) -join "`n"
foreach ($lbl in $blockLabels) {
    if (-not $chunkText.Contains($lbl)) { Abort ('WEBCONSOLE ZIP STALE: SettingsPage chunk missing ' + $lbl) }
}
if (-not $rxBlockArray.IsMatch($chunkText)) {
    Abort 'WEBCONSOLE ZIP STALE: SettingsPage chunk missing the 4-value block-size array'
}
L ('[6] webconsole.zip ' + $chunkName + ' carries 4/8/16/32/64 KB + [4096,8192,16384,32768,65536] (' + $chunkText.Length + ' chars probed)')

# ---- 7) sign Setup: FOUR hash assertions (Setup ONLY) -------------------
if ((SigOf $setup) -ne 'NotSigned') { Abort 'SETUP BASELINE BEFORE SIGNING IS NOT UNSIGNED' }
CopyChecked $setup $stageSetup '[7] setup stage'
$stageHash0 = HashOf $stageSetup
if ($stageHash0 -ne $expectSetup) { Abort ('SETUP STAGE OVERWRITE MISMATCH: ' + $stageHash0) }
if ($stageHash0 -eq $oldSetup) { Abort 'SETUP STAGE STILL HOLDS THE OLD SIGNED IMAGE' }
L ('[7] (a)(b) dist baseline + ASCII stage overwrite OK ' + $stageHash0)

$r7 = Invoke-Step -Tag '[7] signtool setup' -Exe $sigtool -ArgList @('sign','/sm','/s','My','/sha1',$thumb,'/fd','SHA256',$stageSetup)
if ($r7.ExitCode -ne 0) { Abort ('SETUP SIGN FAILED exit=' + $r7.ExitCode) }
if ((SigOf $stageSetup) -ne 'Valid') { Abort 'SETUP SIGNATURE ON STAGE NOT VALID' }
$signedStageHash = HashOf $stageSetup
if ($signedStageHash -eq $expectSetup -or $signedStageHash -eq $oldSetup) {
    Abort 'SIGNED STAGE HASH INVALID (unchanged or old)'
}
L ('[7] (c) signed stage sha256=' + $signedStageHash)

CopyChecked $stageSetup $setup '[7] setup copy-back'
$copyBackHash = HashOf $setup
if ($copyBackHash -ne $signedStageHash) { Abort ('SETUP COPY-BACK MISMATCH: ' + $copyBackHash) }
if ((SigOf $setup) -ne 'Valid') { Abort 'DIST SETUP NOT VALID AFTER COPY-BACK' }
L ('[7] (d) final signed Setup sha256=' + $copyBackHash + ' size=' + (Get-Item -LiteralPath $setup).Length)

# ---- 8) deploy.ps1 (CHILD) -> post-deploy gate5 -------------------------
# [92 3.1 勘误 2026-11-07] 只签 dist 而不部署，重启后内核加载的仍是旧 sys
# （dist 与 System32 两份当前同为 10fb7009…），本次修复等于白做。deploy.ps1
# 是幂等 + verify-before-change + 完整回滚的既有链；其前置 gate 读 sidecar /
# 三锚点做 fail-closed，故必须排在第 2/3 步（dist 已签名 + 三锚点已同步 +
# sidecar 已写）之后——上面的顺序已满足。
# 它不写 marker、不碰 bootcfg；会重写看门基线（DeploySeq+1 / Armed=1 /
# Healthy=0 / BootFails=0）并重注册两个看门任务。重启后必须【交互登录】
# 让 logon_disarm 解除武装，否则 verify_final_reboot.ps1 会假 FAIL。
$r8 = Invoke-Step -Tag '[8] deploy' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$deploy)
if ($r8.ExitCode -ne 0) { Abort ('DEPLOY FAILED exit=' + $r8.ExitCode) }
L ('[8] deploy OK (new sys deployed to System32\drivers; no reboot triggered)')
$deployedSys = Join-Path $env:WINDIR 'System32\drivers\RAMBlockCache.sys'
if (Test-Path -LiteralPath $deployedSys) {
    L ('[8] deployed sys sha256=' + (HashOf $deployedSys) + ' (informational: a locked file may be queued for next-boot replace)')
}
# deploy.ps1:528-530 prints an explicit marker when MoveFileEx queued the
# replace because System32\drivers\RAMBlockCache.sys was locked. Capture it
# so the final scope read can distinguish "deployed" from "queued".
$deployPending = [bool]($r8.Output -match 'queued for replace on next reboot')
if ($deployPending) {
    L '[8] deploy reported a queued MoveFileEx replace-on-reboot (System32 sys was locked).'
    Write-Host '[i] System32 sys replace is queued for next boot; a SECOND reboot may be needed.'
} else {
    L '[8] deploy performed an immediate System32 replacement.'
}
Invoke-Gate5 '[8] gate5 post-deploy'

# ---- 9) ledger append is FATAL + own read-back ---------------------------
# sync_setup_hash.ps1 hard cross-checks the LAST FINAL_SETUP_SHA256 against
# dist Setup and the LAST FINAL_SYS_SHA256 against dist sys. A silent append
# failure would abort the post-UAC documentation backfill.
try {
    Add-Content -LiteralPath $hashLedger -Value ('FINAL_SETUP_SHA256=' + $copyBackHash) -ErrorAction Stop
    Add-Content -LiteralPath $hashLedger -Value ('FINAL_SYS_SHA256=' + $sysHash) -ErrorAction Stop
} catch {
    Abort ('HASH LEDGER APPEND FAILED (fatal: sync_setup_hash.ps1 would mis-compare): ' + $_.Exception.Message)
}
try {
    $lastSetup = @(Get-Content -LiteralPath $hashLedger | Where-Object { $_ -match '^FINAL_SETUP_SHA256=' } | Select-Object -Last 1)
    $lastSys   = @(Get-Content -LiteralPath $hashLedger | Where-Object { $_ -match '^FINAL_SYS_SHA256=' } | Select-Object -Last 1)
} catch {
    Abort ('HASH LEDGER READ-BACK FAILED: ' + $_.Exception.Message)
}
if ($lastSetup.Count -ne 1 -or (($lastSetup[0] -split '=', 2)[1]).Trim().ToLower() -ne $copyBackHash) {
    Abort 'HASH LEDGER READ-BACK MISMATCH (FINAL_SETUP_SHA256 != signed Setup)'
}
if ($lastSys.Count -ne 1 -or (($lastSys[0] -split '=', 2)[1]).Trim().ToLower() -ne $sysHash) {
    Abort 'HASH LEDGER READ-BACK MISMATCH (FINAL_SYS_SHA256 != signed sys)'
}
L ('[9] hash ledger appended + read-back OK ' + $hashLedger)

# ---- 10) final fail-closed scope double-read -----------------------------
if ((HashOf $distSys) -ne $sysHash) { Abort 'SYS HASH DRIFT AT END (expected the NEW signed sys)' }
if ((SigOf $distSys) -ne 'Valid') { Abort 'SYS SIGNATURE NO LONGER VALID' }
try { $sideText2 = ([IO.File]::ReadAllText($sidecar)).Trim() }
catch { Abort ('SYS SIDECAR RE-READ FAILED: ' + $_.Exception.Message) }
if ($sideText2 -ne ('SHA256: ' + $sysHash)) { Abort ('SYS SIDECAR CHANGED: ' + $sideText2) }
$a1 = Get-AnchorState '[10]'
for ($i = 0; $i -lt $anchorFiles.Count; $i++) {
    if ($a1[$i] -ne $sysHash) { Abort ('SYS ANCHOR DRIFT AT END: ' + $anchorFiles[$i]) }
}
try {
    $readmeFinal = [IO.File]::ReadAllText($readme)
} catch { Abort ('README FINAL READ FAILED: ' + $_.Exception.Message) }
if ($rxNorm.Replace($readmeFinal, 'DefaultExpectedSha256 = <HASH>') -ne $readmeBeforeNorm) {
    Abort 'README.txt CHANGED BEYOND THE SYS HASH LINE (final read)'
}
try {
    $markerText1 = ([IO.File]::ReadAllText($marker, (New-Object Text.UTF8Encoding($false)))).Trim()
} catch { Abort ('MARKER RE-READ FAILED: ' + $_.Exception.Message) }
if ($markerText1 -ne $markerText0) { Abort ('MARKER CHANGED (must never be written): ' + $markerText1) }
if ((HashOf $setup) -ne $copyBackHash -or (SigOf $setup) -ne 'Valid') {
    Abort 'FINAL DIST SETUP IS NOT THE SIGNED IMAGE'
}

# [92 3.1 返工] DIRECT EVIDENCE that the fix will be live after reboot: the
# deployed System32 copy must BE the newly signed image (hash + Valid).
# No false FAIL on a locked file: deploy.ps1:516 passes -AllowPendingReplace
# to verify_deploy_signature.ps1, whose :154-161 accepts a matching PFRO
# (source,target) replace-on-reboot pair and exits 0; deploy.ps1:528-530 then
# prints the marker captured above. In that one case we do NOT Abort - we
# corroborate the PFRO queue and emit an actionable second-reboot hint.
if (-not (Test-Path -LiteralPath $deployedSys)) {
    Abort 'DEPLOYED SYS MISSING in System32\drivers (deploy did not land)'
}
$deployedHash = HashOf $deployedSys
$deployedSig  = SigOf $deployedSys
if ($deployPending -and $deployedHash -ne $sysHash) {
    $pfroKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'
    $pfro = (Get-ItemProperty -Path $pfroKey -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
    $pfroHit = $false
    if ($pfro) { $pfroHit = [bool](@($pfro) -match 'RAMBlockCache\.sys') }
    if (-not $pfroHit) {
        Abort 'DEPLOYED SYS HASH MISMATCH and NO PendingFileRenameOperations replace queued - fix would NOT be live after reboot'
    }
    L ('[10] deployed sys still old (' + $deployedHash + ') but a matching replace-on-reboot is queued (PFRO); -AllowPendingReplace accepted it.')
    L '[10] ACTION: reboot ONCE to apply the queued replace; if driver log / GET_STATS still show the old build, reboot ONE more time.'
    Write-Host '[!] System32 sys replace is queued for next boot. A SECOND reboot may be required before S4.'
} else {
    if ($deployedHash -ne $sysHash) { Abort ('DEPLOYED SYS HASH != NEW SIGNED SYS (' + $deployedHash + ') - fix would NOT be live after reboot') }
    if ($deployedSig -ne 'Valid') { Abort ('DEPLOYED SYS SIGNATURE NOT VALID (' + $deployedSig + ')') }
    L ('[10] deployed sys == new signed sys ' + $deployedHash + ' and Valid (fix will be live after reboot)')
}
L ('[10] scope intact: new sys=' + $sysHash + ' + deployed sys verified + sidecar + 3 anchors + README hash-line-only + marker=' + $markerText1 + ' (never written)')

# ---- 11) reboot marker LAST (94 5.2 / B-1+H-7) ---------------------------
# Written ONLY here, after every check above passed, so verify_final_reboot X2
# (boot_utc > finalize_utc) genuinely proves "this UAC actually rebooted".
$markerText = [datetime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture)
try {
    Set-Content -LiteralPath $marker -Value $markerText -Encoding ASCII -ErrorAction Stop
    $mBack = ([IO.File]::ReadAllText($marker, (New-Object Text.UTF8Encoding($false)))).Trim()
} catch { Abort ('MARKER WRITE/READ FAILED: ' + $_.Exception.Message) }
if ($mBack -ne $markerText -or $mBack.Length -ne 24 -or -not $mBack.EndsWith('Z')) {
    Abort ('MARKER VERIFY FAILED: ' + $mBack)
}
L ('[11] reboot marker written LAST utc=' + $markerText)

# ---- 12) ALL OK + NEXT ---------------------------------------------------
L ('ALL OK sys=' + $sysHash + ' setup=' + $copyBackHash)
L 'NEXT: lead runs sync_setup_hash.ps1 -RequireSetupChanged (plain session), then ONE reboot.'
L 'IMPORTANT: after reboot an INTERACTIVE LOGON is REQUIRED (logon_disarm clears FilterArmed);'
L '           without it verify_final_reboot.ps1 gives a deterministic false FAIL.'
L '           If deploy reported a queued replace-on-reboot, a SECOND reboot may be needed.'
L 'Then: verify_final_reboot.ps1 FAIL=0, then the S4 five-tier matrix 4K->8K->16K->32K->64K->4K.'
Write-Host ('=== finalize_rebuild_fix OK sys=' + $sysHash + ' setup=' + $copyBackHash + ' ===')
Write-Host 'NEXT: lead runs sync_setup_hash.ps1 -RequireSetupChanged; reboot ONCE; LOG ON interactively.'
exit 0
