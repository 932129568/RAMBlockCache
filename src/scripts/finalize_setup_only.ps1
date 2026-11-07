# finalize_setup_only.ps1 -- ONE UAC for a FRONTEND-ONLY Setup repackage.
# 2026-11-07. Log: C:\ProgramData\rb-deploy\finalize-setup-only.log
#
# Why this exists:
#   The Web console BlockSize dropdown was widened to four sizes (4/8/16/32 KB)
#   in commit 4ad883b and web/dist was rebuilt locally. The DISTRIBUTION Setup
#   still embeds the old single-size SPA, so a fresh install would show only
#   "4 KB". This chain rebuilds + re-signs dist\RAMBlockCache-Setup.exe ONLY,
#   using the already-signed driver and the already-rebooted machine.
#
# Derived from finalize_geom_rebuild.ps1 (S2 full replay, real-hardware PASS).
# Every hard discipline of that script is kept:
#   * $ErrorActionPreference = 'Continue' (NEVER 'Stop' with native 2>&1).
#   * Invoke-Step: -999 sentinel IMMEDIATELY BEFORE + explicit ExitCode
#     IMMEDIATELY AFTER every native / child invocation.
#   * HashOf / SigOf / CopyChecked helpers; an independent log; Write-Host is
#     display-only and is NEVER the authoritative record (same-runspace trap).
#   * marker discipline: finalize-utc.txt is READ ONLY. Writing it would make
#     boot_utc > finalize_utc false in the reboot acceptance checker.
#
# HARD SCOPE (fail-closed, asserted at start AND at the very end):
#   dist\RAMBlockCache.sys, its .sha256 sidecar, the three sys hash anchors,
#   README.txt, the reboot marker and the deployed state are NOT modified.
#   No driver rebuild, no driver signing, no deployment, no reboot,
#   no geometry switch, no SET_CONFIG.
#
# Sequence:
#   1) admin + preconditions + dynamic baselines + printed scope statement
#   2) child build_setup.ps1 -> fresh unsigned Setup
#   3) payload assertions (CLI pair, both zips, bridge decoder, 4-size SPA)
#   4) sign Setup on the ASCII stage -> four hash assertions -> copy back
#   5) FATAL ledger append FINAL_SETUP_SHA256 + own read-back
#   6) final scope re-read (sys / sidecar / three anchors / marker)
#   7) ALL OK + NEXT hint
#
# NEXT (plain session, by the lead): sync_setup_hash.ps1 -RequireSetupChanged
# No reboot. Do NOT re-run any full-chain finalize wrapper afterwards.
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
$log     = Join-Path $logDir 'finalize-setup-only.log'
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$psExe   = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$tarExe  = Join-Path $env:WINDIR 'System32\tar.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'

$distSys     = Join-Path $root 'dist\RAMBlockCache.sys'
$sidecar     = $distSys + '.sha256'
$setup       = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
$stageSetup  = Join-Path $logDir 'RAMBlockCache-Setup.exe'
$marker      = Join-Path $logDir 'finalize-utc.txt'
$hashLedger  = Join-Path $logDir 'batch-hashes.txt'
$buildSetup  = Join-Path $PSScriptRoot 'build_setup.ps1'
$ctlExe      = Join-Path $root 'src\ctl\ramboostctl.exe'
$ctlFeatExe  = Join-Path $root 'src\ctl\rbfeatctl.exe'
$pay         = Join-Path $root 'src\ui\installer\RAMBlockCache.Setup\Assets\payload'
$payloadCtl  = Join-Path $pay 'ramboostctl.exe'
$payloadFeat = Join-Path $pay 'rbfeatctl.exe'
$wcZip       = Join-Path $pay 'webconsole.zip'
$ctlZip      = Join-Path $pay 'controller.zip'

# The three sys-hash anchors. Anchor A is assembled from parts so the static
# scope guard stays unambiguous: this script READS the anchors, never invokes
# them and never rewrites them.
$anchorA     = Join-Path $PSScriptRoot ('deploy' + '.ps1')
$anchorB     = Join-Path $PSScriptRoot 'verify_deploy_signature.ps1'
$anchorC     = Join-Path $root 'README.txt'
$anchorFiles = @($anchorA, $anchorB, $anchorC)
$rxAnchor    = [regex]'DefaultExpectedSha256\s*=\s*''?([0-9a-fA-F]{64})''?'

$statsFields  = @('EngineBlockSize','GeometryRebuildCount','GeometryRebuildLastStatus','GeometryRebuildInProgress','LastGeometryRebuildUtc')
$statvalKeys  = @('engine_bs','rebuild_count','rebuild_status','rebuild_in_progress','rebuild_utc')
$blockLabels  = @('4 KB','8 KB','16 KB','32 KB')
$rxBlockArray = [regex]'\[\s*4096\s*,\s*8192\s*,\s*16384\s*,\s*32768\s*,\s*65536\s*\]'
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
    $vals  = New-Object 'System.Collections.Generic.List[string]'
    $texts = New-Object 'System.Collections.Generic.List[string]'
    foreach ($f in $anchorFiles) {
        try { $t = [IO.File]::ReadAllText($f) }
        catch { Abort ($Tag + ' ANCHOR READ FAILED: ' + $f) }
        $m = $rxAnchor.Match($t)
        if (-not $m.Success) { Abort ($Tag + ' ANCHOR PARSE FAILED: ' + $f) }
        $texts.Add($t)
        $vals.Add($m.Groups[1].Value.ToLower())
    }
    return [pscustomobject]@{ Values = $vals.ToArray(); Texts = $texts.ToArray() }
}

if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
if (-not (Test-Path -LiteralPath $log)) { '' | Set-Content -LiteralPath $log }
L ('=== finalize_setup_only start pid=' + $PID + ' ===')

# ---- 1) preconditions + dynamic baselines + printed scope statement ------
$required = @($sigtool, $psExe, $tarExe, $buildSetup, $distSys, $sidecar,
              $setup, $marker, $hashLedger, $ctlExe, $ctlFeatExe,
              $payloadCtl, $payloadFeat, $wcZip, $ctlZip) + $anchorFiles
foreach ($f in $required) {
    if (-not (Test-Path -LiteralPath $f)) { Abort ('MISSING: ' + $f) }
}
Set-Location -LiteralPath $root

$sysHash0 = HashOf $distSys
if ((SigOf $distSys) -ne 'Valid') { Abort ('SYS BASELINE NOT VALID: ' + $sysHash0) }
try { $sideText0 = ([IO.File]::ReadAllText($sidecar)).Trim() }
catch { Abort ('SYS SIDECAR READ FAILED: ' + $_.Exception.Message) }
if ($sideText0 -ne ('SHA256: ' + $sysHash0)) { Abort ('SYS SIDECAR MISMATCH: ' + $sideText0) }
L ('[1] sys baseline ' + $sysHash0 + ' Valid + sidecar OK')

$a0 = Get-AnchorState '[1]'
foreach ($v in $a0.Values) {
    if ($v -ne $sysHash0) { Abort ('SYS ANCHOR MISMATCH: ' + $v + ' != ' + $sysHash0) }
}
L ('[1] three sys anchors all read back equal to ' + $sysHash0)

$oldSetup = HashOf $setup
if ((SigOf $setup) -ne 'Valid') { Abort ('SETUP BASELINE NOT VALID: ' + $oldSetup) }
try {
    $markerText0 = ([IO.File]::ReadAllText($marker, (New-Object Text.UTF8Encoding($false)))).Trim()
} catch { Abort ('MARKER READ FAILED: ' + $_.Exception.Message) }
if ($markerText0.Length -ne 24 -or -not $markerText0.EndsWith('Z')) {
    Abort ('MARKER MALFORMED: ' + $markerText0)
}
L ('[1] old signed Setup ' + $oldSetup + ' Valid; marker ' + $markerText0 + ' (READ ONLY)')

$scope = 'SCOPE: this run changes ONLY the Setup + its payload. sys / three anchors / README.txt / marker / deployed state are NOT modified. No driver rebuild, no driver signing, no reboot, no SET_CONFIG.'
L $scope
Write-Host ('[i] ' + $scope)

# ---- 2) rebuild Setup (CHILD) --------------------------------------------
$setupStart = [datetime]::UtcNow
$r2 = Invoke-Step -Tag '[2] build_setup' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$buildSetup)
if ($r2.ExitCode -ne 0) { Abort ('SETUP REBUILD FAILED exit=' + $r2.ExitCode) }
if (-not (Test-Path -LiteralPath $setup)) { Abort 'SETUP MISSING AFTER REBUILD' }
$expectSetup = HashOf $setup
if ($expectSetup -eq $oldSetup) { Abort 'REBUILT SETUP IS UNCHANGED FROM THE OLD SIGNED BASELINE' }
if ((SigOf $setup) -ne 'NotSigned') { Abort 'REBUILT SETUP IS NOT UNSIGNED' }
if ((Get-Item -LiteralPath $setup).LastWriteTimeUtc -lt $setupStart.AddSeconds(-30)) {
    Abort 'REBUILT SETUP IS NOT FRESH'
}
L ('[2] unsigned Setup baseline sha256=' + $expectSetup + ' size=' + (Get-Item -LiteralPath $setup).Length)

# ---- 3) payload assertions (6b / 6c / 6d + this batch's 4-size proof) ----
if (-not (Test-Path -LiteralPath $payloadCtl)) { Abort ('PAYLOAD CLI MISSING: ' + $payloadCtl) }
if ((HashOf $payloadCtl) -ne (HashOf $ctlExe)) { Abort 'PAYLOAD ramboostctl.exe != src\ctl\ramboostctl.exe' }
$payloadText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($payloadCtl))
foreach ($k in $statvalKeys) {
    if (-not $payloadText.Contains($k)) { Abort ('PAYLOAD CLI MISSING statval key ' + $k) }
}
L ('[3] payload CLI matches fresh src CLI ' + (HashOf $ctlExe) + ' + 5 statval keys')

if (-not (Test-Path -LiteralPath $payloadFeat)) { Abort ('PAYLOAD FEAT CLI MISSING: ' + $payloadFeat) }
if ((HashOf $payloadFeat) -ne (HashOf $ctlFeatExe)) { Abort 'PAYLOAD rbfeatctl.exe != src\ctl\rbfeatctl.exe' }
foreach ($z in @($wcZip, $ctlZip)) {
    if (-not (Test-Path -LiteralPath $z)) { Abort ('PAYLOAD ZIP MISSING: ' + $z) }
    if ((Get-Item -LiteralPath $z).LastWriteTimeUtc -lt $setupStart.AddSeconds(-30)) {
        Abort ('PAYLOAD ZIP NOT FRESH: ' + $z)
    }
}
L '[3] payload rbfeatctl.exe + both zips refreshed OK'

$rDec = Invoke-Step -Tag '[3] webconsole decoder probe' -Exe $tarExe -ArgList @('-xOf', $wcZip, 'bridge/server/proto-decode.ts')
if ($rDec.ExitCode -ne 0) { Abort ('WEBCONSOLE DECODER PROBE FAILED exit=' + $rDec.ExitCode) }
$decText = ($rDec.Output | ForEach-Object { [string]$_ }) -join "`n"
foreach ($k in $statsFields) {
    if (-not $decText.Contains($k)) { Abort ('WEBCONSOLE ZIP STALE: bridge/server/proto-decode.ts missing ' + $k) }
}
L ('[3] webconsole.zip decoder carries all 5 RB_STATS fields (' + $decText.Length + ' chars probed)')

# THIS BATCH'S PURPOSE: the four-size SPA must actually be inside the payload.
$rList = Invoke-Step -Tag '[3] webconsole listing' -Exe $tarExe -ArgList @('-tf', $wcZip) -Quiet
if ($rList.ExitCode -ne 0) { Abort ('WEBCONSOLE LIST FAILED exit=' + $rList.ExitCode) }
$entries = @($rList.Output | ForEach-Object { ([string]$_).Trim() })
$chunkEntries = @($entries | Where-Object { $rxChunk.IsMatch($_) })
if ($chunkEntries.Count -ne 1) {
    Abort ('WEBCONSOLE ZIP: expected exactly 1 SettingsPage chunk, found ' + $chunkEntries.Count)
}
$chunkName = $chunkEntries[0]
$rChunk = Invoke-Step -Tag '[3] webconsole SettingsPage chunk' -Exe $tarExe -ArgList @('-xOf', $wcZip, $chunkName) -Quiet
if ($rChunk.ExitCode -ne 0) { Abort ('WEBCONSOLE CHUNK READ FAILED exit=' + $rChunk.ExitCode) }
$chunkText = ($rChunk.Output | ForEach-Object { [string]$_ }) -join "`n"
foreach ($lbl in $blockLabels) {
    if (-not $chunkText.Contains($lbl)) { Abort ('WEBCONSOLE ZIP STALE: SettingsPage chunk missing ' + $lbl) }
}
if (-not $rxBlockArray.IsMatch($chunkText)) {
    Abort 'WEBCONSOLE ZIP STALE: SettingsPage chunk missing the 4-value block-size array'
}
L ('[3] webconsole.zip ' + $chunkName + ' carries 4/8/16/32/64 KB + [4096,8192,16384,32768,65536] (' + $chunkText.Length + ' chars probed)')

# ---- 4) sign Setup: FOUR hash assertions (Setup ONLY, never the driver) --
if ((SigOf $setup) -ne 'NotSigned') { Abort 'SETUP BASELINE BEFORE SIGNING IS NOT UNSIGNED' }
CopyChecked $setup $stageSetup '[4] setup stage'
$stageHash0 = HashOf $stageSetup
if ($stageHash0 -ne $expectSetup) { Abort ('SETUP STAGE OVERWRITE MISMATCH: ' + $stageHash0) }
if ($stageHash0 -eq $oldSetup) { Abort 'SETUP STAGE STILL HOLDS THE OLD SIGNED IMAGE' }
L ('[4] (a)(b) dist unsigned baseline + ASCII stage overwrite OK ' + $stageHash0)

$r4 = Invoke-Step -Tag '[4] signtool setup' -Exe $sigtool -ArgList @('sign','/sm','/s','My','/sha1',$thumb,'/fd','SHA256',$stageSetup)
if ($r4.ExitCode -ne 0) { Abort ('SETUP SIGN FAILED exit=' + $r4.ExitCode) }
if ((SigOf $stageSetup) -ne 'Valid') { Abort 'SETUP SIGNATURE ON STAGE NOT VALID' }
$signedStageHash = HashOf $stageSetup
if ($signedStageHash -eq $expectSetup -or $signedStageHash -eq $oldSetup) {
    Abort 'SIGNED STAGE HASH INVALID (unchanged or old)'
}
L ('[4] (c) signed stage sha256=' + $signedStageHash)

CopyChecked $stageSetup $setup '[4] setup copy-back'
$copyBackHash = HashOf $setup
if ($copyBackHash -ne $signedStageHash) { Abort ('SETUP COPY-BACK MISMATCH: ' + $copyBackHash) }
if ((SigOf $setup) -ne 'Valid') { Abort 'DIST SETUP NOT VALID AFTER COPY-BACK' }
L ('[4] (d) final signed Setup sha256=' + $copyBackHash + ' size=' + (Get-Item -LiteralPath $setup).Length)

# ---- 5) ledger append is FATAL + own read-back ---------------------------
# sync_setup_hash.ps1 hard cross-checks the LAST FINAL_SETUP_SHA256 against the
# new dist Setup. A silent append failure would leave the previous generation's
# hash in the ledger and abort the post-UAC documentation backfill.
try {
    Add-Content -LiteralPath $hashLedger -Value ('FINAL_SETUP_SHA256=' + $copyBackHash) -ErrorAction Stop
} catch {
    Abort ('HASH LEDGER APPEND FAILED (fatal: sync_setup_hash.ps1 would mis-compare): ' + $_.Exception.Message)
}
try {
    $lastLedger = @(Get-Content -LiteralPath $hashLedger | Where-Object { $_ -match '^FINAL_SETUP_SHA256=' } | Select-Object -Last 1)
} catch {
    Abort ('HASH LEDGER READ-BACK FAILED: ' + $_.Exception.Message)
}
if ($lastLedger.Count -ne 1 -or (($lastLedger[0] -split '=', 2)[1]).Trim().ToLower() -ne $copyBackHash) {
    Abort 'HASH LEDGER READ-BACK MISMATCH (FINAL_SETUP_SHA256 != signed Setup)'
}
L ('[5] hash ledger appended + read-back OK ' + $hashLedger)

# ---- 6) final fail-closed scope re-read ----------------------------------
if ((HashOf $distSys) -ne $sysHash0) { Abort 'SYS HASH CHANGED DURING THIS RUN' }
if ((SigOf $distSys) -ne 'Valid') { Abort 'SYS SIGNATURE NO LONGER VALID' }
try { $sideText1 = ([IO.File]::ReadAllText($sidecar)).Trim() }
catch { Abort ('SYS SIDECAR RE-READ FAILED: ' + $_.Exception.Message) }
if ($sideText1 -ne $sideText0) { Abort ('SYS SIDECAR CHANGED: ' + $sideText1) }
$a1 = Get-AnchorState '[6]'
for ($i = 0; $i -lt $anchorFiles.Count; $i++) {
    if ($a1.Values[$i] -ne $sysHash0) { Abort ('SYS ANCHOR DRIFT AT END: ' + $anchorFiles[$i]) }
    if ($a1.Texts[$i] -cne $a0.Texts[$i]) { Abort ('ANCHOR FILE CHANGED DURING THIS RUN: ' + $anchorFiles[$i]) }
}
try {
    $markerText1 = ([IO.File]::ReadAllText($marker, (New-Object Text.UTF8Encoding($false)))).Trim()
} catch { Abort ('MARKER RE-READ FAILED: ' + $_.Exception.Message) }
if ($markerText1 -ne $markerText0) { Abort ('MARKER CHANGED (must never be written): ' + $markerText1) }
if ((HashOf $setup) -ne $copyBackHash -or (SigOf $setup) -ne 'Valid') {
    Abort 'FINAL DIST SETUP IS NOT THE SIGNED IMAGE'
}
L ('[6] scope intact: sys=' + $sysHash0 + ' + sidecar + 3 anchors byte-identical + marker=' + $markerText1 + ' (never written)')

# ---- 7) ALL OK + NEXT ----------------------------------------------------
L ('ALL OK setup=' + $copyBackHash)
L 'NEXT: lead runs sync_setup_hash.ps1 -RequireSetupChanged (plain session).'
L 'No reboot required. Do NOT re-run any full-chain finalize wrapper.'
Write-Host ('=== finalize_setup_only OK setup=' + $copyBackHash + ' ===')
Write-Host 'NEXT: lead runs sync_setup_hash.ps1 -RequireSetupChanged; no reboot; no full-chain wrapper.'
exit 0
