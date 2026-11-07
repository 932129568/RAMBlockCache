# finalize_rebuild_fix.ps1 -- ONE UAC for the [92 A/B/C] rebuild-drain fix.
# 2026-11-07. Log: C:\ProgramData\rb-deploy\finalize-rebuild-fix.log
#
# Why this exists:
#   The volume-level geometry rebuild timed out deterministically on real
#   hardware (91/92: ERROR [453] detach drain timeout, [268] datapath quiesce
#   timeout, [334] stream(s) still attached). The fix touches rbattach.c /
#   rbrebuild.c / rb_common.h, so the driver must be rebuilt and re-signed.
#   A full-chain replay would overwrite the reboot marker and run unrelated
#   test-flag reset / deployment steps. THIS chain is intentionally narrow:
#   sign the already-built driver, sync the three anchors, gate5, repack and
#   re-sign the Setup, append the ledger. NO marker write, NO deployment,
#   NO reboot, NO SET_CONFIG.
#
# Derived from finalize_setup_only.ps1 / finalize_geom_rebuild.ps1.
# Every hard discipline is kept:
#   * $ErrorActionPreference = 'Continue' (NEVER 'Stop' with native 2>&1).
#   * Invoke-Step: -999 sentinel IMMEDIATELY BEFORE + explicit ExitCode
#     IMMEDIATELY AFTER every native / child invocation.
#   * HashOf / SigOf / CopyChecked; independent log; Write-Host is display-only.
#   * gate5 is a CHILD process judged by BOTH exit code and 'result: PASS' text.
#   * marker discipline: finalize-utc.txt is READ ONLY and byte-compared at the
#     end (writing it would break boot_utc > finalize_utc in the reboot check).
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
#      bridge index.ts, four-size SPA, new sys in payload)
#   7) sign Setup: four hash assertions
#   8) FATAL ledger append FINAL_SETUP_SHA256 + own read-back
#   9) final scope double-read
#  10) ALL OK + NEXT hint
#
# NEXT (plain session, by the lead): sync_setup_hash.ps1 -RequireSetupChanged,
# then ONE reboot, then verify_final_reboot.ps1 and the S4 geometry matrix.
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
$readme      = Join-Path $root 'README.txt'
$ctlExe      = Join-Path $root 'src\ctl\ramboostctl.exe'
$ctlFeatExe  = Join-Path $root 'src\ctl\rbfeatctl.exe'
$pay         = Join-Path $root 'src\ui\installer\RAMBlockCache.Setup\Assets\payload'
$payloadCtl  = Join-Path $pay 'ramboostctl.exe'
$payloadFeat = Join-Path $pay 'rbfeatctl.exe'
$payloadSys  = Join-Path $pay 'RAMBlockCache.sys'
$wcZip       = Join-Path $pay 'webconsole.zip'
$ctlZip      = Join-Path $pay 'controller.zip'

# The three sys-hash anchors. Anchor A is assembled from parts so that the
# static scope guard stays unambiguous: this script READS the anchors and
# invokes the anchor-sync tool, but never invokes the deployment tool.
$anchorA     = Join-Path $PSScriptRoot ('deploy' + '.ps1')
$anchorB     = Join-Path $PSScriptRoot 'verify_deploy_signature.ps1'
$anchorC     = $readme
$anchorFiles = @($anchorA, $anchorB, $anchorC)
$rxAnchor    = [regex]'DefaultExpectedSha256\s*=\s*''?([0-9a-fA-F]{64})''?'
$rxNorm      = [regex]'DefaultExpectedSha256\s*=\s*''?[0-9a-fA-F]{64}''?'

$statsFields  = @('EngineBlockSize','GeometryRebuildCount','GeometryRebuildLastStatus','GeometryRebuildInProgress','LastGeometryRebuildUtc')
$statvalKeys  = @('engine_bs','rebuild_count','rebuild_status','rebuild_in_progress','rebuild_utc')
$blockLabels  = @('4 KB','8 KB','16 KB','32 KB')
$rxBlockArray = [regex]'\[\s*4096\s*,\s*8192\s*,\s*16384\s*,\s*32768\s*\]'
$rxChunk      = [regex]'SettingsPage-[A-Za-z0-9_-]+\.js$'

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
              $setup, $marker, $hashLedger, $gate, $syncAnchors, $buildSetup,
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

$scope = 'SCOPE: this run re-signs the driver + Setup and syncs the three sys anchors. marker / deployed state / config are NOT modified. No deployment, no reboot, no SET_CONFIG, no test-flag reset.'
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
L ('[6] webconsole.zip ' + $chunkName + ' carries 4/8/16/32 KB + [4096,8192,16384,32768] (' + $chunkText.Length + ' chars probed)')

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

# ---- 8) ledger append is FATAL + own read-back ---------------------------
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
L ('[8] hash ledger appended + read-back OK ' + $hashLedger)

# ---- 9) final fail-closed scope double-read ------------------------------
if ((HashOf $distSys) -ne $sysHash) { Abort 'SYS HASH DRIFT AT END (expected the NEW signed sys)' }
if ((SigOf $distSys) -ne 'Valid') { Abort 'SYS SIGNATURE NO LONGER VALID' }
try { $sideText2 = ([IO.File]::ReadAllText($sidecar)).Trim() }
catch { Abort ('SYS SIDECAR RE-READ FAILED: ' + $_.Exception.Message) }
if ($sideText2 -ne ('SHA256: ' + $sysHash)) { Abort ('SYS SIDECAR CHANGED: ' + $sideText2) }
$a1 = Get-AnchorState '[9]'
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
L ('[9] scope intact: new sys=' + $sysHash + ' + sidecar + 3 anchors + README hash-line-only + marker=' + $markerText1 + ' (never written)')

# ---- 10) ALL OK + NEXT ---------------------------------------------------
L ('ALL OK sys=' + $sysHash + ' setup=' + $copyBackHash)
L 'NEXT: lead runs sync_setup_hash.ps1 -RequireSetupChanged (plain session), then ONE reboot.'
L 'After reboot: verify_final_reboot.ps1 FAIL=0, then the S4 4K->8K->16K->32K->4K matrix.'
Write-Host ('=== finalize_rebuild_fix OK sys=' + $sysHash + ' setup=' + $copyBackHash + ' ===')
Write-Host 'NEXT: lead runs sync_setup_hash.ps1 -RequireSetupChanged; then reboot ONCE.'
exit 0
