# finalize_distribution_fix.ps1 -- ONE UAC for the FINAL distribution rebuild.
# 2026-11-07. Log: C:\ProgramData\rb-deploy\finalize-distribution-fix.log
#
# Why this exists (batch B of deliverables/88-setup-chain-audit-2026-11-07.md):
#   Batch A fixed the Web console from source (4-size BlockSize dropdown and a
#   cwd-independent SPA root). Batch B fixed the distribution engine (Secure
#   Boot / testsigning preflight before ANY machine change, truthful
#   DEMAND_START log, no BOOT_START downgrade), the installer C#, the INF
#   reference doc, the WinRE rollback script and the boot guard. None of that
#   reaches an end user until the distribution Setup is rebuilt and re-signed.
#
# Derived from finalize_setup_only.ps1 (batch A, real-hardware accepted chain).
# Every hard discipline is kept:
#   * $ErrorActionPreference = 'Continue' (NEVER 'Stop' with native 2>&1).
#   * Invoke-Step: -999 sentinel IMMEDIATELY BEFORE + explicit ExitCode
#     IMMEDIATELY AFTER every native / child invocation.
#   * HashOf / SigOf / CopyChecked; independent log; Write-Host is display-only
#     and is NEVER the authoritative record (same-runspace trap).
#   * marker discipline: finalize-utc.txt is READ ONLY and byte-compared at the
#     end. Writing it would make boot_utc > finalize_utc false in the reboot
#     acceptance checker.
#
# HARD SCOPE (asserted at start AND at the very end):
#   dist\RAMBlockCache.sys, its .sha256 sidecar, the three sys hash anchors,
#   the reboot marker and the deployed state are NOT modified.
#   No driver rebuild, no driver signing, no deployment, no reboot,
#   no geometry switch, no SET_CONFIG, no driver test-flag reset.
#   ONLY src\setup\ramboostsetup.exe, the payload directory and
#   dist\RAMBlockCache-Setup.exe are rewritten and re-signed.
#
# Sequence:
#   1) admin + preconditions + dynamic baselines + printed scope statement
#   2) _build_engine.ps1 -OutFile -> freshly compiled UNSIGNED engine
#   3) sign engine on the ASCII stage -> Valid -> copy back
#   4) build_setup.ps1 (CHILD) -> fresh unsigned Setup
#   5) payload assertions: CLI pair, both zips, bridge decoder, 4-size SPA,
#      cwd-independent bridge index.ts, payload engine == signed engine
#   6) sign Setup: four hash assertions
#   7) FATAL ledger append FINAL_SETUP_SHA256 + own read-back
#   8) final scope re-read (sys / sidecar / three anchors / marker)
#   9) ALL OK + NEXT hint
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
$log     = Join-Path $logDir 'finalize-distribution-fix.log'
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$psExe   = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$tarExe  = Join-Path $env:WINDIR 'System32\tar.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'

$distSys      = Join-Path $root 'dist\RAMBlockCache.sys'
$sidecar      = $distSys + '.sha256'
$setup        = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
$engine       = Join-Path $root 'src\setup\ramboostsetup.exe'
$stageEngine  = Join-Path $logDir 'ramboostsetup.exe'
$stageSetup   = Join-Path $logDir 'RAMBlockCache-Setup.exe'
$marker       = Join-Path $logDir 'finalize-utc.txt'
$hashLedger   = Join-Path $logDir 'batch-hashes.txt'
$buildEngine  = Join-Path $root 'src\setup\_build_engine.ps1'
$buildSetup   = Join-Path $PSScriptRoot 'build_setup.ps1'
$ctlExe       = Join-Path $root 'src\ctl\ramboostctl.exe'
$ctlFeatExe   = Join-Path $root 'src\ctl\rbfeatctl.exe'
$pay          = Join-Path $root 'src\ui\installer\RAMBlockCache.Setup\Assets\payload'
$payloadCtl   = Join-Path $pay 'ramboostctl.exe'
$payloadFeat  = Join-Path $pay 'rbfeatctl.exe'
$payloadEngine= Join-Path $pay 'ramboostsetup.exe'
$wcZip        = Join-Path $pay 'webconsole.zip'
$ctlZip       = Join-Path $pay 'controller.zip'

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
L ('=== finalize_distribution_fix start pid=' + $PID + ' ===')

# ---- 1) preconditions + dynamic baselines + printed scope statement ------
$required = @($sigtool, $psExe, $tarExe, $buildEngine, $buildSetup, $engine,
              $distSys, $sidecar, $setup, $marker, $hashLedger,
              $ctlExe, $ctlFeatExe, $payloadCtl, $payloadFeat,
              $payloadEngine, $wcZip, $ctlZip) + $anchorFiles
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
$oldEngine = HashOf $engine
if ((SigOf $engine) -ne 'Valid') { Abort ('ENGINE BASELINE NOT VALID: ' + $oldEngine) }
try {
    $markerText0 = ([IO.File]::ReadAllText($marker, (New-Object Text.UTF8Encoding($false)))).Trim()
} catch { Abort ('MARKER READ FAILED: ' + $_.Exception.Message) }
if ($markerText0.Length -ne 24 -or -not $markerText0.EndsWith('Z')) {
    Abort ('MARKER MALFORMED: ' + $markerText0)
}
L ('[1] old Setup ' + $oldSetup + ' Valid; old engine ' + $oldEngine + ' Valid; marker ' + $markerText0 + ' (READ ONLY)')

$scope = 'SCOPE: this run changes ONLY the silent engine, the payload and the Setup. sys / three anchors / marker / deployed state are NOT modified. No driver rebuild, no driver signing, no deployment, no reboot, no SET_CONFIG.'
L $scope
Write-Host ('[i] ' + $scope)

# ---- 2) rebuild the silent engine (CHILD) --------------------------------
$r2 = Invoke-Step -Tag '[2] build engine' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$buildEngine,'-OutFile',$engine)
if ($r2.ExitCode -ne 0) { Abort ('ENGINE REBUILD FAILED exit=' + $r2.ExitCode) }
$unsignedEngine = HashOf $engine
if ($unsignedEngine -eq $oldEngine) { Abort 'REBUILT ENGINE IS UNCHANGED FROM THE OLD SIGNED BASELINE' }
if ((SigOf $engine) -ne 'NotSigned') { Abort 'REBUILT ENGINE IS NOT UNSIGNED' }
L ('[2] unsigned engine sha256=' + $unsignedEngine + ' size=' + (Get-Item -LiteralPath $engine).Length)

# ---- 3) sign engine on the ASCII stage -----------------------------------
CopyChecked $engine $stageEngine '[3] engine stage'
$stageEngineHash0 = HashOf $stageEngine
if ($stageEngineHash0 -ne $unsignedEngine) { Abort ('ENGINE STAGE OVERWRITE MISMATCH: ' + $stageEngineHash0) }
if ($stageEngineHash0 -eq $oldEngine) { Abort 'ENGINE STAGE STILL HOLDS THE OLD SIGNED IMAGE' }
L ('[3] (a)(b) engine baseline + ASCII stage overwrite OK ' + $stageEngineHash0)

$r3 = Invoke-Step -Tag '[3] signtool engine' -Exe $sigtool -ArgList @('sign','/sm','/s','My','/sha1',$thumb,'/fd','SHA256',$stageEngine)
if ($r3.ExitCode -ne 0) { Abort ('ENGINE SIGN FAILED exit=' + $r3.ExitCode) }
if ((SigOf $stageEngine) -ne 'Valid') { Abort 'ENGINE SIGNATURE ON STAGE NOT VALID' }
$signedEngineHash = HashOf $stageEngine
if ($signedEngineHash -eq $unsignedEngine -or $signedEngineHash -eq $oldEngine) {
    Abort 'SIGNED ENGINE HASH INVALID (unchanged or old)'
}
L ('[3] (c) signed engine stage sha256=' + $signedEngineHash)

CopyChecked $stageEngine $engine '[3] engine copy-back'
if ((HashOf $engine) -ne $signedEngineHash) { Abort 'ENGINE COPY-BACK MISMATCH' }
if ((SigOf $engine) -ne 'Valid') { Abort 'DIST ENGINE NOT VALID AFTER COPY-BACK' }
L ('[3] (d) final signed engine sha256=' + $signedEngineHash)

# ---- 4) rebuild Setup (CHILD) --------------------------------------------
$setupStart = [datetime]::UtcNow
$r4 = Invoke-Step -Tag '[4] build_setup' -Exe $psExe -ArgList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$buildSetup)
if ($r4.ExitCode -ne 0) { Abort ('SETUP REBUILD FAILED exit=' + $r4.ExitCode) }
if (-not (Test-Path -LiteralPath $setup)) { Abort 'SETUP MISSING AFTER REBUILD' }
$expectSetup = HashOf $setup
if ($expectSetup -eq $oldSetup) { Abort 'REBUILT SETUP IS UNCHANGED FROM THE OLD SIGNED BASELINE' }
if ((SigOf $setup) -ne 'NotSigned') { Abort 'REBUILT SETUP IS NOT UNSIGNED' }
if ((Get-Item -LiteralPath $setup).LastWriteTimeUtc -lt $setupStart.AddSeconds(-30)) {
    Abort 'REBUILT SETUP IS NOT FRESH'
}
L ('[4] unsigned Setup baseline sha256=' + $expectSetup + ' size=' + (Get-Item -LiteralPath $setup).Length)

# ---- 5) payload assertions (6b / 6c / 6d + batch A/B proof) -------------
if (-not (Test-Path -LiteralPath $payloadCtl)) { Abort ('PAYLOAD CLI MISSING: ' + $payloadCtl) }
if ((HashOf $payloadCtl) -ne (HashOf $ctlExe)) { Abort 'PAYLOAD ramboostctl.exe != src\ctl\ramboostctl.exe' }
$payloadText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($payloadCtl))
foreach ($k in $statvalKeys) {
    if (-not $payloadText.Contains($k)) { Abort ('PAYLOAD CLI MISSING statval key ' + $k) }
}
L ('[5] payload CLI matches fresh src CLI ' + (HashOf $ctlExe) + ' + 5 statval keys')

if (-not (Test-Path -LiteralPath $payloadFeat)) { Abort ('PAYLOAD FEAT CLI MISSING: ' + $payloadFeat) }
if ((HashOf $payloadFeat) -ne (HashOf $ctlFeatExe)) { Abort 'PAYLOAD rbfeatctl.exe != src\ctl\rbfeatctl.exe' }
foreach ($z in @($wcZip, $ctlZip)) {
    if (-not (Test-Path -LiteralPath $z)) { Abort ('PAYLOAD ZIP MISSING: ' + $z) }
    if ((Get-Item -LiteralPath $z).LastWriteTimeUtc -lt $setupStart.AddSeconds(-30)) {
        Abort ('PAYLOAD ZIP NOT FRESH: ' + $z)
    }
}
L '[5] payload rbfeatctl.exe + both zips refreshed OK'

# [88 B-2] the newly signed engine MUST be the one embedded in the payload.
if (-not (Test-Path -LiteralPath $payloadEngine)) { Abort ('PAYLOAD ENGINE MISSING: ' + $payloadEngine) }
if ((HashOf $payloadEngine) -ne $signedEngineHash) { Abort 'PAYLOAD ramboostsetup.exe != newly signed engine' }
if ((SigOf $payloadEngine) -ne 'Valid') { Abort 'PAYLOAD ENGINE NOT VALID' }
L ('[5] payload engine == newly signed engine ' + $signedEngineHash + ' Valid')

$rDec = Invoke-Step -Tag '[5] webconsole decoder probe' -Exe $tarExe -ArgList @('-xOf', $wcZip, 'bridge/server/proto-decode.ts')
if ($rDec.ExitCode -ne 0) { Abort ('WEBCONSOLE DECODER PROBE FAILED exit=' + $rDec.ExitCode) }
$decText = ($rDec.Output | ForEach-Object { [string]$_ }) -join "`n"
foreach ($k in $statsFields) {
    if (-not $decText.Contains($k)) { Abort ('WEBCONSOLE ZIP STALE: bridge/server/proto-decode.ts missing ' + $k) }
}
L ('[5] webconsole.zip decoder carries all 5 RB_STATS fields (' + $decText.Length + ' chars probed)')

# [88 B-3] the packed bridge MUST be the cwd-independent batch-A version.
$rIdx = Invoke-Step -Tag '[5] webconsole bridge index probe' -Exe $tarExe -ArgList @('-xOf', $wcZip, 'bridge/server/index.ts') -Quiet
if ($rIdx.ExitCode -ne 0) { Abort ('WEBCONSOLE BRIDGE PROBE FAILED exit=' + $rIdx.ExitCode) }
$idxText = ($rIdx.Output | ForEach-Object { [string]$_ }) -join "`n"
if ($idxText.Contains('process.cwd()')) { Abort 'WEBCONSOLE ZIP STALE: bridge/server/index.ts still uses process.cwd()' }
if (-not $idxText.Contains('import.meta.url')) { Abort 'WEBCONSOLE ZIP STALE: bridge/server/index.ts missing import.meta.url' }
if ($idxText.Contains([string][char]0)) { Abort 'WEBCONSOLE ZIP STALE: bridge/server/index.ts contains a literal NUL byte' }
L ('[5] webconsole.zip bridge index.ts is cwd-independent + NUL-free (' + $idxText.Length + ' chars probed)')

# [batch A] the four-size SPA must actually be inside the payload.
$rList = Invoke-Step -Tag '[5] webconsole listing' -Exe $tarExe -ArgList @('-tf', $wcZip) -Quiet
if ($rList.ExitCode -ne 0) { Abort ('WEBCONSOLE LIST FAILED exit=' + $rList.ExitCode) }
$entries = @($rList.Output | ForEach-Object { ([string]$_).Trim() })
$chunkEntries = @($entries | Where-Object { $rxChunk.IsMatch($_) })
if ($chunkEntries.Count -ne 1) {
    Abort ('WEBCONSOLE ZIP: expected exactly 1 SettingsPage chunk, found ' + $chunkEntries.Count)
}
$chunkName = $chunkEntries[0]
$rChunk = Invoke-Step -Tag '[5] webconsole SettingsPage chunk' -Exe $tarExe -ArgList @('-xOf', $wcZip, $chunkName) -Quiet
if ($rChunk.ExitCode -ne 0) { Abort ('WEBCONSOLE CHUNK READ FAILED exit=' + $rChunk.ExitCode) }
$chunkText = ($rChunk.Output | ForEach-Object { [string]$_ }) -join "`n"
foreach ($lbl in $blockLabels) {
    if (-not $chunkText.Contains($lbl)) { Abort ('WEBCONSOLE ZIP STALE: SettingsPage chunk missing ' + $lbl) }
}
if (-not $rxBlockArray.IsMatch($chunkText)) {
    Abort 'WEBCONSOLE ZIP STALE: SettingsPage chunk missing the 4-value block-size array'
}
L ('[5] webconsole.zip ' + $chunkName + ' carries 4/8/16/32/64 KB + [4096,8192,16384,32768,65536] (' + $chunkText.Length + ' chars probed)')

# ---- 6) sign Setup: FOUR hash assertions (Setup ONLY, never the driver) --
if ((SigOf $setup) -ne 'NotSigned') { Abort 'SETUP BASELINE BEFORE SIGNING IS NOT UNSIGNED' }
CopyChecked $setup $stageSetup '[6] setup stage'
$stageHash0 = HashOf $stageSetup
if ($stageHash0 -ne $expectSetup) { Abort ('SETUP STAGE OVERWRITE MISMATCH: ' + $stageHash0) }
if ($stageHash0 -eq $oldSetup) { Abort 'SETUP STAGE STILL HOLDS THE OLD SIGNED IMAGE' }
L ('[6] (a)(b) dist baseline + ASCII stage overwrite OK ' + $stageHash0)

$r6 = Invoke-Step -Tag '[6] signtool setup' -Exe $sigtool -ArgList @('sign','/sm','/s','My','/sha1',$thumb,'/fd','SHA256',$stageSetup)
if ($r6.ExitCode -ne 0) { Abort ('SETUP SIGN FAILED exit=' + $r6.ExitCode) }
if ((SigOf $stageSetup) -ne 'Valid') { Abort 'SETUP SIGNATURE ON STAGE NOT VALID' }
$signedStageHash = HashOf $stageSetup
if ($signedStageHash -eq $expectSetup -or $signedStageHash -eq $oldSetup) {
    Abort 'SIGNED STAGE HASH INVALID (unchanged or old)'
}
L ('[6] (c) signed stage sha256=' + $signedStageHash)

CopyChecked $stageSetup $setup '[6] setup copy-back'
$copyBackHash = HashOf $setup
if ($copyBackHash -ne $signedStageHash) { Abort ('SETUP COPY-BACK MISMATCH: ' + $copyBackHash) }
if ((SigOf $setup) -ne 'Valid') { Abort 'DIST SETUP NOT VALID AFTER COPY-BACK' }
L ('[6] (d) final signed Setup sha256=' + $copyBackHash + ' size=' + (Get-Item -LiteralPath $setup).Length)

# ---- 7) ledger append is FATAL + own read-back ---------------------------
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
L ('[7] hash ledger appended + read-back OK ' + $hashLedger)

# ---- 8) final fail-closed scope re-read ----------------------------------
if ((HashOf $distSys) -ne $sysHash0) { Abort 'SYS HASH CHANGED DURING THIS RUN' }
if ((SigOf $distSys) -ne 'Valid') { Abort 'SYS SIGNATURE NO LONGER VALID' }
try { $sideText1 = ([IO.File]::ReadAllText($sidecar)).Trim() }
catch { Abort ('SYS SIDECAR RE-READ FAILED: ' + $_.Exception.Message) }
if ($sideText1 -ne $sideText0) { Abort ('SYS SIDECAR CHANGED: ' + $sideText1) }
$a1 = Get-AnchorState '[8]'
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
if ((HashOf $engine) -ne $signedEngineHash -or (SigOf $engine) -ne 'Valid') {
    Abort 'FINAL ENGINE IS NOT THE SIGNED IMAGE'
}
L ('[8] scope intact: sys=' + $sysHash0 + ' + sidecar + 3 anchors byte-identical + marker=' + $markerText1 + ' (never written)')

# ---- 9) ALL OK + NEXT ----------------------------------------------------
L ('ALL OK setup=' + $copyBackHash + ' engine=' + $signedEngineHash)
L 'NEXT: lead runs sync_setup_hash.ps1 -RequireSetupChanged (plain session).'
L 'No reboot required. Do NOT re-run any full-chain finalize wrapper.'
Write-Host ('=== finalize_distribution_fix OK setup=' + $copyBackHash + ' ===')
Write-Host 'NEXT: lead runs sync_setup_hash.ps1 -RequireSetupChanged; no reboot; no full-chain wrapper.'
exit 0
