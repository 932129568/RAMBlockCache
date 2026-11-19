# RB-postreboot-verify.ps1  [2026-11-15]
# READ-ONLY post-reboot verification for the RAMBlockCache deploy chain.
# Runs AFTER: src\scripts\deploy.cmd  ->  ONE reboot  ->  RB-postreboot-verify.cmd
#
# READ-ONLY posture: no refresh / no flags write / no l2 enable / no shrink /
# no rollback / no reboot / no bootcfg POST / no registry write / no file copy.
# The only thing this script writes is its own stdout (the caller redirects it to
# C:\ProgramData\rb-deploy\postreboot-verify.log).
#
# WHY cmdlet-free hashing: see deliverables/244. On one host Get-FileHash was not
# resolvable at all, which made the deploy pre-flight gate fail closed. Hashing here
# uses .NET directly so this script cannot fail for that reason.
#
# DUAL PATH for registry / service: PowerShell provider + CIM first (works on any
# normal console), native reg.exe / sc.exe as fallback (works where the module
# subsystem is degraded). Either path yields the same verdict.
#
# Exit code = number of HARD failures (0 = live image landed, sig valid, service
# boot-start+running, UpperFilters registered, at least one volume accelerated).
#
# [SUP-01 2026-11-17] Adds a READ-ONLY shutdown-persist readout section (the
# "SUP01 raw / pair / verdict / limit-*" lines and one bare
# "SUP01-VERDICT: <CODE>" machine line for the P1-P4 power-cycle drivers).
# It is INFO only by design: it never touches PASS= / FAIL= / WARN=, so
# "exit $script:fail" keeps exactly the meaning it had before this change.
# Section code sits immediately above the "=== VERDICT ===" block.

$ErrorActionPreference = 'Continue'

# [FIX T3 2026-11-15] PIN is a DEPLOY BASELINE, not a permanent constant. A
# re-sign replaces the deployed image and sync_anchors.ps1 moves all three
# anchors; if this literal is not moved too, C1/C1b fail with a misleading
# "image mismatch" even though live==dist==anchors and Authenticode is Valid.
# C0 (below) fails closed on any future drift instead of blaming the image.
$PIN  = '2dce9d720e5708ed3486df9dec6b0dce72d4d1fc9d6c35d5b66fab387a8aed05'
$CLS  = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'
$SVC  = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache'
$SM   = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'

$repo = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $repo 'dist\RAMBlockCache.sys'
$live = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
$ctl  = Join-Path $repo 'src\ctl\rbfeatctl.exe'

$script:pass = 0
$script:fail = 0
$script:warn = 0

function Ok([string]$Id, [string]$Msg)  { Write-Output ('[PASS] ' + $Id + ' ' + $Msg); $script:pass++ }
function No([string]$Id, [string]$Msg)  { Write-Output ('[FAIL] ' + $Id + ' ' + $Msg); $script:fail++ }
function Wn([string]$Id, [string]$Msg)  { Write-Output ('[WARN] ' + $Id + ' ' + $Msg); $script:warn++ }
function Inf([string]$Id, [string]$Msg) { Write-Output ('[info] ' + $Id + ' ' + $Msg) }

function Get-Sha256Hex([string]$Path) {
    $fs = [System.IO.File]::Open($Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::ReadWrite)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $bytes = $sha.ComputeHash($fs)
            return ([System.BitConverter]::ToString($bytes) -replace '-', '').ToLowerInvariant()
        } finally { $sha.Dispose() }
    } finally { $fs.Dispose() }
}

function Run([string]$Exe, [string[]]$ArgList) {
    $text = ''
    $code = 1
    try {
        $out = & $Exe @ArgList 2>&1
        $code = $LASTEXITCODE
        foreach ($l in $out) { $text = $text + [string]$l + "`n" }
    } catch {
        $text = 'invoke failed: ' + $_.Exception.Message
        $code = 900
    }
    return [pscustomobject]@{ Code = $code; Text = $text }
}

# Registry read: provider first, native reg.exe fallback. Returns raw value text or $null.
function Get-RegRaw([string]$Key, [string]$Name) {
    try {
        $p = Get-ItemProperty -LiteralPath $Key -Name $Name -ErrorAction Stop
        $v = $p.$Name
        if ($null -ne $v) {
            if ($v -is [array]) { return ($v -join "`n") }
            return [string]$v
        }
    } catch { }
    $native = $Key.Replace('HKLM:\', 'HKLM\').Replace('HKCU:\', 'HKCU\')
    $r = Run 'reg.exe' @('query', $native, '/v', $Name)
    if ($r.Code -ne 0) { return $null }
    foreach ($l in ($r.Text -split "`n")) {
        if ($l -match ('\s' + [regex]::Escape($Name) + '\s+REG_')) { return $l.Trim() }
    }
    return $null
}

function Get-FlagsInt([string]$Raw) {
    # [295 P2-1 2026-11-19] 原实现 ToInt32(...,16)：0xFFFFFFFF 静默返回 Int32 -1，
    # 而 -1 -eq [uint32]::MaxValue 两个方向都为 False ⇒ 「Flags=0xFFFFFFFF（全开）」
    # 会被读成一个既不等于全开、也不等于任何具名位的怪物值，判据永远不报错。
    # 改成 ToUInt32，与本文件 Get-Dword 同口径。
    # hex 分支必须**独占** 0x 前缀并显式判宽：早先写成 `-match '0x(...{1,8})'`
    # 会把 0x1FFFFFFFF **前缀截断**成 536870911（实测），退化成下一条的 0（实测），
    # 两种都是静默读错。「判据不得静默跳过」同样禁止「静默读错」⇒ 超宽一律 $null。
    if (-not $Raw) { return $null }
    if ($Raw -match '0[xX]([0-9a-fA-F]+)') {
        if ($Matches[1].Length -gt 8) { return $null }
        return [Convert]::ToUInt32($Matches[1], 16)
    }
    if ($Raw -match '(-?\d+)') { return [uint32](([int64]$Matches[1]) -band 0xFFFFFFFFL) }
    return $null
}

# Read a DWORD as [uint32]. Returns $null when the value is absent/unreadable.
# NOTE: deliberately NOT Get-FlagsInt -- that one goes through
# [Convert]::ToInt32(...,16), which returns Int32 -1 for 0xFFFFFFFF (only >32-bit
# throws) and -1 -ne [uint32]::MaxValue. This one returns [uint32].
function Get-Dword([string]$Key, [string]$Name) {
    $raw = Get-RegRaw $Key $Name
    if ($null -eq $raw) { return $null }
    if ($raw -match '0x([0-9a-fA-F]{1,8})') { return [Convert]::ToUInt32($Matches[1], 16) }
    if ($raw -match '(-?\d+)') { return [uint32](([int64]$Matches[1]) -band 0xFFFFFFFFL) }
    return $null
}

# ---------------------------------------------------------------- banner / env
$hostEngine = 'PowerShell ' + $PSVersionTable.PSVersion.ToString()
if ($PSVersionTable.PSEdition) { $hostEngine = $hostEngine + ' / ' + $PSVersionTable.PSEdition }
Write-Output '=== RAMBlockCache POST-REBOOT VERIFY (READ-ONLY) ==='
Write-Output ('host engine  : ' + $hostEngine)
Write-Output ('utc          : ' + (Get-Date).ToUniversalTime().ToString('o'))
try {
    $bt = (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime
    Write-Output ('last boot    : ' + $bt.ToUniversalTime().ToString('o'))
} catch {
    Write-Output ('last boot    : UNKNOWN (' + $_.Exception.Message + ')')
}
$isAdmin = $false
try {
    $pr = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    $isAdmin = $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) { Write-Output 'elevated     : TRUE' }
    else { Write-Output 'elevated     : FALSE (run RB-postreboot-verify.cmd, it self-elevates)' }
} catch { Write-Output 'elevated     : UNKNOWN' }

# ------------------------------------- C0 baseline coherence (hard, ADDS a check)
# The verifier PIN must equal the dist sidecar AND both deploy-script anchors.
# Empty match sets are a FAIL (never a PASS): a missing anchor is a broken
# baseline, not a clean one. This check only adds constraints; it relaxes none.
$anchorFiles = @(
    (Join-Path $repo 'dist\RAMBlockCache.sys.sha256'),
    (Join-Path $repo 'src\scripts\deploy.ps1'),
    (Join-Path $repo 'src\scripts\verify_deploy_signature.ps1')
)
$anchorBad = @()
foreach ($af in $anchorFiles) {
    $leaf = Split-Path -Leaf $af
    if (-not (Test-Path -LiteralPath $af)) { $anchorBad += ($leaf + ': missing'); continue }
    try {
        $txt = [IO.File]::ReadAllText($af)
        $hit = [regex]::Match($txt, '(?i)\b([0-9a-f]{64})\b')
        if (-not $hit.Success) { $anchorBad += ($leaf + ': no 64-hex hash'); continue }
        $got = $hit.Groups[1].Value.ToLowerInvariant()
        if ($got -ne $PIN) { $anchorBad += ($leaf + ': ' + $got) }
    } catch { $anchorBad += ($leaf + ': ' + $_.Exception.Message) }
}
if ($anchorBad.Count -eq 0) { Ok 'C0' ('verifier PIN == sidecar == 2 deploy anchors (' + $PIN.Substring(0,8) + '...)') }
else { No 'C0' ('PIN/anchor drift -> re-sync the verifier PIN: ' + ($anchorBad -join '; ')) }

# ------------------------------------------------------- C1 image identity (hard)
$liveHash = $null
if (-not (Test-Path -LiteralPath $live)) {
    No 'C1' ('live sys missing: ' + $live)
} else {
    try {
        $liveHash = Get-Sha256Hex $live
        $li = Get-Item -LiteralPath $live
        Inf 'C1' ('live  ' + $liveHash + '  ' + $li.Length + ' B  ' + $li.LastWriteTimeUtc.ToString('o'))
        if ($liveHash -eq $PIN) { Ok 'C1' ('live SHA256 == pin ' + $PIN.Substring(0,8) + '...') }
        else { No 'C1' ('live SHA256 != pin (live=' + $liveHash + ')') }
    } catch { No 'C1' ('cannot hash live sys: ' + $_.Exception.Message) }
}
if (-not (Test-Path -LiteralPath $dist)) {
    No 'C1b' ('dist sys missing: ' + $dist)
} else {
    try {
        $distHash = Get-Sha256Hex $dist
        $di = Get-Item -LiteralPath $dist
        Inf 'C1b' ('dist  ' + $distHash + '  ' + $di.Length + ' B')
        if ($distHash -ne $PIN) { No 'C1b' 'dist SHA256 != pin' }
        elseif ($liveHash -eq $distHash) { Ok 'C1b' 'live image == dist image' }
        else { No 'C1b' 'live image != dist image (replace queued or not applied)' }
    } catch { No 'C1b' ('cannot hash dist sys: ' + $_.Exception.Message) }
}

# --------------------------------------------------- C2 signature of live image
$sigOk = $false
$sigTxt = 'UNKNOWN'
if (Get-Command Get-AuthenticodeSignature -ErrorAction SilentlyContinue) {
    try {
        $sig = Get-AuthenticodeSignature -LiteralPath $live -ErrorAction Stop
        $sigTxt = $sig.Status.ToString()
        if ($sig.Status -eq 'Valid') { $sigOk = $true }
    } catch { $sigTxt = 'cmdlet failed: ' + $_.Exception.Message }
} else { $sigTxt = 'cmdlet unavailable' }
Inf 'C2' ('Get-AuthenticodeSignature -> ' + $sigTxt)

if (-not $sigOk) {
    $signtool = $null
    $c = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($c) { $signtool = $c.Source }
    if (-not $signtool) {
        foreach ($r in @((Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'), (Join-Path $env:ProgramFiles 'Windows Kits\10\bin'))) {
            if ($r -and (Test-Path -LiteralPath $r)) {
                $hit = Get-ChildItem -LiteralPath $r -Recurse -Filter 'signtool.exe' -ErrorAction SilentlyContinue |
                       Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
                       Sort-Object FullName -Descending | Select-Object -First 1
                if ($hit) { $signtool = $hit.FullName; break }
            }
        }
    }
    if ($signtool) {
        $v = Run $signtool @('verify', '/pa', $live)
        Inf 'C2' ('signtool verify /pa -> exit ' + $v.Code)
        if ($v.Code -eq 0) { $sigOk = $true; $sigTxt = 'Valid (signtool /pa)' }
    } else { Wn 'C2' 'signtool.exe not found; cannot cross-check' }
}
if ($sigOk) { Ok 'C2' ('Authenticode of live image: ' + $sigTxt) }
else { No 'C2' ('Authenticode of live image NOT valid: ' + $sigTxt) }

# ------------------------------------------------ C3 service boot-start+running
# Three independent paths, in order of reliability on this machine:
#   1) Win32_SystemDriver  - a driver, not a service; returns StartMode/State
#   2) Get-Service         - StartType/Status (PS 5.1+)
#   3) sc.exe qc/query     - native fallback
# NOTE: Win32_Service returned an EMPTY object on this machine (Name/StartMode/State
# all blank) - that is why it is not used. "Could not query" must never be reported
# as "service missing": those are different verdicts.
$svcStart = $null
$svcState = $null
$from = ''
$probeErr = @()
try {
    $d = Get-CimInstance Win32_SystemDriver -Filter "Name='RAMBlockCache'" -ErrorAction Stop
    if ($d -and $d.StartMode) { $svcStart = [string]$d.StartMode; $svcState = [string]$d.State; $from = 'Win32_SystemDriver' }
} catch { $probeErr += ('Win32_SystemDriver: ' + $_.Exception.Message) }
if (-not $from) {
    try {
        $g = Get-Service -Name 'RAMBlockCache' -ErrorAction Stop
        if ($g) { $svcState = [string]$g.Status; $svcStart = [string]$g.StartType; $from = 'Get-Service' }
    } catch { $probeErr += ('Get-Service: ' + $_.Exception.Message) }
}
if (-not $from) {
    $qc = Run 'sc.exe' @('qc', 'RAMBlockCache')
    $qs = Run 'sc.exe' @('query', 'RAMBlockCache')
    if ($qc.Code -eq 0) {
        $from = 'sc.exe'
        if ($qc.Text -match 'START_TYPE\s*:\s*(\d+)\s*(\S+)') { $svcStart = $Matches[1] + ' ' + $Matches[2] }
        if ($qs.Text -match 'STATE\s*:\s*(\d+)\s+(\S+)') { $svcState = $Matches[1] + ' ' + $Matches[2] }
    } elseif ($qc.Code -eq 900) {
        $probeErr += 'sc.exe could not be executed (blocked or missing)'
    } else {
        $from = 'sc.exe'
        $svcStart = 'missing'
        $svcState = 'missing'
    }
}
Inf 'C3' ('source=' + $(if ($from) { $from } else { 'none' }) + '  START=' + $svcStart + '  STATE=' + $svcState)
if ($probeErr.Count -gt 0) { foreach ($e in $probeErr) { Inf 'C3' ('  ' + $e) } }
if (-not $from) {
    No 'C3a' 'could not query the service state by any path (see lines above)'
    No 'C3b' 'could not query the service state by any path (see lines above)'
} elseif ($svcStart -eq 'missing') {
    No 'C3a' 'RAMBlockCache service key not found'
    No 'C3b' 'RAMBlockCache service key not found'
} else {
    if ($svcStart -match 'Boot|BOOT_START') { Ok 'C3a' ('service start type = ' + $svcStart) }
    else { No 'C3a' ('service start type = ' + $svcStart + ' (expected Boot / 0 BOOT_START)') }
    if ($svcState -match 'Running|RUNNING') { Ok 'C3b' 'service running (driver resident)' }
    else { No 'C3b' ('service state = ' + $svcState + ' (driver NOT loaded)') }
}

# --------------------------------------------------- C4 volume-class UpperFilters
$uf = Get-RegRaw $CLS 'UpperFilters'
if (-not $uf) {
    No 'C4' 'UpperFilters value absent on the volume class key'
} else {
    $flat = ($uf -replace "`r?`n", ' ').Trim()
    Inf 'C4' ('UpperFilters -> ' + $flat)
    if ($flat -match 'RAMBlockCache') { Ok 'C4' 'RAMBlockCache registered in UpperFilters' }
    else { No 'C4' 'RAMBlockCache NOT in UpperFilters (PnP never calls RbAddDevice -> volumes stay 0)' }
}

# ------------------------------------------- C5 attached volumes (the decisive bit)
$volN = $null
if (-not (Test-Path -LiteralPath $ctl)) {
    No 'C5' ('rbfeatctl.exe missing: ' + $ctl)
} else {
    $v = Run $ctl @('vols')
    Inf 'C5' ('rbfeatctl vols -> exit ' + $v.Code)
    foreach ($l in ($v.Text -split "`n")) { if ($l.Trim()) { Inf 'C5' ('  ' + $l.TrimEnd()) } }
    if ($v.Text -match 'volumes:\s*(\d+)') {
        $volN = [int]$Matches[1]
        if ($volN -ge 1) { Ok 'C5' ('volumes=' + $volN + ' (L1/L2 has a live volume)') }
        else { No 'C5' 'volumes=0 (driver resident but no volume attached -> no acceleration)' }
    } else {
        if ($isAdmin) { No 'C5' ('cannot parse "volumes:" (rbfeatctl exit ' + $v.Code + ')') }
        else { No 'C5' ('cannot read volume status while not elevated (rbfeatctl exit ' + $v.Code + ')') }
    }
}

# ------------------------------------------------------- context: boot config
foreach ($n in @('AutoAttach', 'ResumeOnBoot', 'ResumeLetters', 'FilterArmed', 'BootFailCount', 'LastBootHealthy', 'DeploySeq', 'DeployAck')) {
    $raw = Get-RegRaw ($SVC + '\Parameters') $n
    if ($raw) { Inf 'P' ($n + ' = ' + (($raw -replace '\s+', ' ').Trim())) }
    else { Inf 'P' ($n + ' = <absent>') }
}
$flagRaw = Get-RegRaw ($SVC + '\Features') 'Flags'
$flags = Get-FlagsInt $flagRaw
if ($null -ne $flags) { Inf 'P' ('Features\Flags = ' + $flags + ' (0x' + $flags.ToString('X') + ')') }
else { Inf 'P' 'Features\Flags = <absent>' }

# --------------------------------------------- C6 pending replace leftovers
$pf = Get-RegRaw $SM 'PendingFileRenameOperations'
if (-not $pf) { Ok 'C6' 'no PendingFileRenameOperations (no replace queued)' }
elseif ($pf -match 'RAMBlockCache\.sys') { No 'C6' 'PendingFileRenameOperations still references RAMBlockCache.sys (replace not applied)' }
else { Ok 'C6' 'PendingFileRenameOperations present but no RAMBlockCache.sys entry' }

# ------------------------------------------------------- context: driver log tail
$dl = Join-Path $env:ProgramData ('RAMBlockCache\logs\driver-' + (Get-Date -Format 'yyyyMMdd') + '.log')
if (Test-Path -LiteralPath $dl) {
    Inf 'L' ('tail of ' + $dl)
    try { Get-Content -LiteralPath $dl -Tail 25 -ErrorAction Stop | ForEach-Object { Inf 'L' ('  ' + $_) } }
    catch { Inf 'L' ('read failed: ' + $_.Exception.Message) }
} else { Inf 'L' ('no driver log for today: ' + $dl) }

# -------------------------------------------------------------- stats snapshot
if (Test-Path -LiteralPath $ctl) {
    $sj = Run $ctl @('stats', '-json')
    Inf 'S' ('rbfeatctl stats -json -> exit ' + $sj.Code)
    foreach ($l in ($sj.Text -split "`n")) { if ($l.Trim()) { Inf 'S' ('  ' + $l.TrimEnd()) } }
}

# --------------------------------- context: boot-loop safety net (INFO only)
# deploy.ps1 steps 4/5 install these. Missing tasks do not break the driver today,
# but they remove the auto-disarm safety net for a future failed boot.
$pd = Join-Path $env:ProgramData 'RAMBoost'
foreach ($g in @('boot_guard.ps1', 'logon_disarm.ps1')) {
    $gp = Join-Path $pd $g
    if (Test-Path -LiteralPath $gp) { Inf 'G' ($g + ' present (' + (Get-Item -LiteralPath $gp).Length + ' B)') }
    else { Wn 'G' ($g + ' MISSING in ' + $pd) }
}
foreach ($t in @('RAMBoostBootGuard', 'RAMBoostLogonDisarm')) {
    try {
        $x = Get-ScheduledTask -TaskName $t -ErrorAction Stop
        Inf 'G' ('task ' + $t + ' present (state=' + $x.State + ')')
    } catch {
        Wn 'G' ('task ' + $t + ' NOT visible to this account; re-run deploy.cmd to register the boot-loop safety net')
    }
}

# ------------------------------------ SUP-01 shutdown-persist readout (INFO only)
# The driver leaves its shutdown-persist evidence in the service Parameters key
# for the NEXT boot to read. This section reads that pair and maps it onto the
# 11-row table of the SUP-01 design note (section 3.5). Every number below is
# copied verbatim from that table -- do not "tidy" it up here.
#
# [A4-d 2026-11-17] Two values below are NOT from that note: $W_WITHHELD (7) and
# $S_CLEAN_WITHHELD (9). They come from `274`: the driver now reports a withheld
# clean as RB_PERSIST_WHY_WITHHELD=7 mapped to RB_SKIP_CLEAN_WITHHELD=9. Without
# the matching row the pair (7,9) falls through to 'UNKNOWN', i.e. the main
# acceptance tool cannot see the one outcome A4-d exists to expose. The three
# copies of this table (rbmsg.c switch / this file / read_shutdown_sentinel.py)
# are pinned together by src/scripts/check_sup01_tables.py (Gate24).
#
# [WHY EVERY LINE BELOW GOES THROUGH Inf AND NEVER Ok/No/Wn]
# This is a READER, not a new verdict. "exit $script:fail" is the deploy chain's
# decision point. A perfectly legal outcome -- e.g. P4 (power cut at shutdown),
# which parks the pair at (0xFF, 0xFFFFFFFF) -- would turn into a deploy failure
# if it were reported through No. So this section must not move
# PASS= / FAIL= / WARN= by even one.
#
# [THE RULE THAT SPLITS THE TWO HARDEST CELLS]
# WhyPrev == 0xFF AND SkipPrev != 0xFFFFFFFF  =>  the record can only have come
# from the fallback branch. The success path always writes Why; the fallback
# branch writes Skip only and never writes Why. So (0xFF, 0x00000008) does not
# mean "unknown why" -- it means "stop-tick failed, fallback flush ran".
#
# [WHY (0xFF, 0xFFFFFFFF) IS NOT DECIDABLE HERE]
# Three readings are indistinguishable: IRP never dispatched / dispatched but at
# IRQL>PASSIVE / power cut mid-shutdown. DiagShutdownSeq cannot split them
# either: the sentinel bump and the Seq bump both live inside the same PASSIVE
# gate in rbmsg.c, so at IRQL>PASSIVE neither of them moves.
#
# [WHY Get-Dword AND NOT Get-FlagsInt]
# Get-FlagsInt does NOT throw on 0xFFFFFFFF -- it silently returns Int32 -1
# (measured). That is worse than throwing: in PowerShell -1 -eq [uint32]::MaxValue
# is False in BOTH directions, so every RB_SKIP_NONE reading (0xFFFFFFFF) would
# quietly classify as UNKNOWN and never raise an error. Get-Dword returns [uint32]
# and compares correctly.

# ShutdownPersistWhy codes (the WHY list).
$W_CLEAN     = [uint32]0
$W_DIRTY     = [uint32]1
$W_FROZEN    = [uint32]2
$W_BUSY      = [uint32]3
$W_IRQL      = [uint32]4
$W_ALREADY   = [uint32]5
# NOTE: the WHY list names 6 "DISABLED"; the verdict table pairs (6, 6) with
# CFG_CLEAN. One value, two names in the source note -- kept as-is on purpose.
$W_CFG_CLEAN = [uint32]6
# [A4-d] clean 被卷数门扣留（本引导从未挂过卷）：不是数据故障，只是没宣告 clean。
$W_WITHHELD  = [uint32]7
# 0xFF is PersistSkipReason's sibling sentinel: "unknown / not reached this boot".
$W_NONE      = [uint32]0xFF

# PersistSkipReason codes (the SKIP list).
$S_IRQL      = [uint32]1
$S_FROZEN    = [uint32]2
$S_BUSY      = [uint32]3
$S_LATCH     = [uint32]4
$S_ALREADY   = [uint32]5
$S_CFG_CLEAN = [uint32]6
$S_DIRTY     = [uint32]7
$S_STOPTICK  = [uint32]8
# [A4-d] 与 $W_WITHHELD 配对的跳过码（rb_common.h RB_SKIP_CLEAN_WITHHELD）。
$S_CLEAN_WITHHELD = [uint32]9
# 0xFFFFFFFF is PersistSkipReason's boot-reset value. Must stay [uint32]:
# routing it through [int] / Get-FlagsInt yields Int32 -1, and -1 -eq [uint32]::MaxValue
# is False -- the "no skip" reading would silently become UNKNOWN.
$S_NONE      = [uint32]::MaxValue

# One fixed-width cell per DWORD, so the log stays diffable: '0x000000FF' or '(absent)'.
function Format-Dword([object]$Value) {
    if ($null -eq $Value) { return '(absent)' }
    return ('0x' + ([uint32]$Value).ToString('X8'))
}

# Aligned '[info] SUP01 <field> ...' line; field padded so the four rows line up.
function Out-Sup01([string]$Field, [string]$Msg) {
    Inf 'SUP01' ($Field.PadRight(11) + $Msg)
}

$supKey   = $SVC + '\Parameters'
$why      = Get-Dword $supKey 'ShutdownPersistWhy'
$whyPrev  = Get-Dword $supKey 'ShutdownPersistWhyPrev'
$skip     = Get-Dword $supKey 'PersistSkipReason'
$skipPrev = Get-Dword $supKey 'PersistSkipReasonPrev'
$seq      = Get-Dword $supKey 'DiagShutdownSeq'

# -1- raw readout: the five values exactly as they sit in the registry right now.
Out-Sup01 'raw' ('Why=' + (Format-Dword $why) +
                 ' WhyPrev=' + (Format-Dword $whyPrev) +
                 ' Skip=' + (Format-Dword $skip) +
                 ' SkipPrev=' + (Format-Dword $skipPrev) +
                 ' Seq=' + $(if ($null -eq $seq) { '(absent)' } else { [string]$seq }))

# -2- the pair, shaped like the table it is judged against (WhyPrev as X2, Skip as X8).
$whyPrevTxt = '(absent)'
if ($null -ne $whyPrev) { $whyPrevTxt = '0x' + $whyPrev.ToString('X2') }
$skipPrevTxt = '(absent)'
if ($null -ne $skipPrev) { $skipPrevTxt = '0x' + $skipPrev.ToString('X8') }
Out-Sup01 'pair' ('(WhyPrev, SkipReasonPrev) = (' + $whyPrevTxt + ', ' + $skipPrevTxt + ')')

# -3- verdict. Order matters: the WhyPrev==0xFF rows are the fallback branch and
#     must be matched before the plain rows, or (0xFF, 6) would land on CFG_CLEAN.
$code = ''
$zh   = ''
if ($null -eq $whyPrev -and $null -eq $skipPrev) {
    $code = 'KEY_ABSENT'
    $zh   = '首次安装后的第一次开机：两个 *Prev 键还没被写过'
} elseif ($null -eq $whyPrev -or $null -eq $skipPrev) {
    $code = 'UNKNOWN'
    $zh   = '成对读数不完整：一个 *Prev 键在、另一个缺，原样打印两个值供人工判读'
} elseif (($whyPrev -eq $W_NONE) -and ($skipPrev -ne $S_NONE)) {
    # WhyPrev==0xFF with a real Skip => fallback branch, never the success path.
    if ($skipPrev -eq $S_STOPTICK) {
        $code = 'FALLBACK_STOPTICK_FAIL'
        $zh   = '兜底分支：tick 没停住，只做了兜底 flush'
    } elseif ($skipPrev -eq $S_CFG_CLEAN) {
        $code = 'FALLBACK_CFG_CLEAN'
        $zh   = '兜底分支：tick 没停住 + mode=2，连 flush 都跳过（配置要求，不是失败）'
    } elseif ($skipPrev -eq $S_FROZEN) {
        $code = 'FALLBACK_FROZEN'
        $zh   = '兜底分支：tick 没停住 + 冻结，flush 跳过（引擎可能已拆）'
    } else {
        $code = 'FALLBACK_OTHER'
        $zh   = '兜底分支，但跳过码不在预期集合内（值可疑）'
    }
} elseif (($whyPrev -eq $W_NONE) -and ($skipPrev -eq $S_NONE)) {
    $code = 'NO_EVIDENCE'
    $zh   = '三义不可分：本次关机 IRP 没派发 / 派发了但 IRQL>PASSIVE / 关机瞬间掉电'
} elseif (($whyPrev -eq $W_CLEAN) -and ($skipPrev -eq $S_NONE)) {
    $code = 'CLEAN'
    $zh   = '落盘成功且干净'
} elseif (($whyPrev -eq $W_DIRTY) -and ($skipPrev -eq $S_DIRTY)) {
    $code = 'DIRTY'
    $zh   = '刷完仍有脏页'
} elseif (($whyPrev -eq $W_FROZEN) -and ($skipPrev -eq $S_FROZEN)) {
    $code = 'FROZEN'
    $zh   = '数据路径冻结，持久化入口即返回'
} elseif (($whyPrev -eq $W_BUSY) -and ($skipPrev -eq $S_BUSY)) {
    $code = 'BUSY'
    $zh   = '另一线程正在落盘（CAS 没抢到）'
} elseif (($whyPrev -eq $W_ALREADY) -and ($skipPrev -eq $S_ALREADY)) {
    $code = 'ALREADY'
    $zh   = '本次关机早前已落过盘（闩=DONE）'
} elseif (($whyPrev -eq $W_CFG_CLEAN) -and ($skipPrev -eq $S_CFG_CLEAN)) {
    $code = 'CFG_CLEAN'
    $zh   = '按配置跳过 clean（ShutdownPersistMode != 0）'
} elseif (($whyPrev -eq $W_WITHHELD) -and ($skipPrev -eq $S_CLEAN_WITHHELD)) {
    $code = 'WITHHELD'
    $zh   = 'clean 被卷数门扣留（本引导从未挂过卷）：不是数据故障，只是没宣告 clean；恢复会话里也可能是闩陈旧+上次被扣（274 D2）'
} elseif (($whyPrev -eq $W_IRQL) -and ($skipPrev -eq $S_IRQL)) {
    $code = 'IRQL_UNWRITABLE'
    $zh   = 'IRQL>PASSIVE 那一路写不进注册表，实际读数会退回 NO_EVIDENCE'
} else {
    $code = 'UNKNOWN'
    $zh   = '不在表内，原样打印两个值供人工判读'
}
Out-Sup01 'verdict' ($code + ' — ' + $zh)

# -4- machine line for the P1-P4 power-cycle drivers: bare, no prefix, and always
#     emitted (a missing line then means "the script died", not "nothing to say").
Write-Output ('SUP01-VERDICT: ' + $code)

# -5- the two limits, printed on EVERY run, not only when they bite.
Out-Sup01 'limit-1' '(0xFF, 0xFFFFFFFF) 三义不可分：本次关机 IRP 没派发 / 派发了但 IRQL>PASSIVE / 关机瞬间掉电。DiagShutdownSeq 也分不开 —— 哨兵与 Seq 的推进同在 rbmsg.c 的 PASSIVE 门内，IRQL>PASSIVE 时两者都不动。'
Out-Sup01 'limit-2' 'RB_SKIP_IRQL(1) 物理不可观测：该路的注册表写点本身在 IRQL>PASSIVE 下静默失败，实际读数退回 (0xFF, 0xFFFFFFFF)。'

# ------------------------------------------------------------------- verdict
Write-Output ''
Write-Output '=== VERDICT ==='
Write-Output ('PASS=' + $script:pass + ' FAIL=' + $script:fail + ' WARN=' + $script:warn)
if ($script:fail -eq 0) {
    Write-Output 'RESULT: POST-REBOOT VERIFY PASSED (image landed, driver resident, volume attached)'
} else {
    Write-Output 'RESULT: POST-REBOOT VERIFY FAILED (see [FAIL] lines above)'
    if ($volN -eq 0) {
        Write-Output 'HINT: volumes=0 with C3/C4 green means the driver is resident but no volume was attached.'
        Write-Output '      Check AutoAttach/ResumeOnBoot above; attaching a volume is a separate step (not done here).'
    }
}
exit $script:fail
