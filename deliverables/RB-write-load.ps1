# RB-write-load.ps1 - 0x7F write/metadata path load generator
#
# WHAT THIS COVERS: the two 2026-11-19 0x7F / DOUBLE_FAULT blue screens came from
# a 1480 B stack frame in RbDispatchDeviceControl.  `ramboostctl bench` has since
# proven the READ path survives 43.4 GB twice.  This script covers the MISSING
# HALF: IRP_MJ_WRITE plus NTFS metadata churn (create / allocate / copy / delete)
# at >=900 MB per round, 2 rounds, no reboot in between.
#
# RED LINES (hard-coded, not configurable):
#   * never touches  D:\大模型   - user's model files, unrecoverable if lost
#   * never writes to a volume root; must land under D:\rb-write-test\
#   * never touches the historical rbc-*.bin leftovers at D:\ root
#   * read-only on Features\Flags / WriteBack / Verifier / UpperFilters
#   * no deploy, no reboot, no git commit
#
# WHY NOT `fsutil file createnew` (measured on this box):
#   fsutil sparse queryflag    -> "NOT set as sparse"   (so it is not sparse)
#   fsutil file queryvaliddata -> "0x0 (0)"             (VDL = 0)
# VDL=0 means the data section was never written: createnew allocates clusters
# and records a size but emits no IRP_MJ_WRITE.  A load built on it would test
# nothing.  So we write real bytes and then ASSERT VDL == expected.
#
# WHY FLUSH TWICE (file + whole volume):
#   Copy-Item runs at ~1227 MB/s here, which means the page cache absorbed it.
#   A per-handle Flush($true) pushes that FILE's data down, but does NOT force
#   the NTFS metadata ($MFT / directory entries) through the filter -- and the
#   metadata path is one of the real 0x7F trigger scenarios.  So after the copy
#   we also flush the whole volume.  `fsutil volume flush` needs elevation
#   (Error 5 otherwise); `Write-VolumeCache -DriveLetter` succeeds unelevated
#   but cannot prove it did anything useful, so elevation is required anyway.
#
# ELEVATION (R8 spirit: one UAC per task):
#   The script self-elevates exactly once via `Start-Process -Verb RunAs` and
#   then does EVERYTHING inside that one elevated session: baseline verifier,
#   both load rounds, post checks.  It never asks twice.
#
# DELETION MUST FREE CLUSTERS, NOT JUST UNLINK:
#   NTFS frees clusters lazily.  We assert (a) the file is really gone and
#   (b) free space on the volume actually grew by ~the deleted size.

[CmdletBinding()]
param(
    [ValidateSet(1, 2)][int]$Rounds = 2,
    [int]$SizeMB = 900,
    [string]$TargetDir = 'D:\rb-write-test',
    [switch]$DryRun,
    [switch]$SkipVerifier,
    [string]$LogPath = '',
    [switch]$NoElevate   # internal: set when the elevated child re-invokes us
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ------------------------------------------------ native-output encoding (P4)
# ramboostctl.exe prints UTF-8, but the console default here is GB2312
# (measured: [Console]::OutputEncoding.WebName = 'gb2312', OEMCP = gb2312).
# Reading its stdout without pinning the encoding turns '写字节' into mojibake
# (measured: '[错误]' arrived as '[閿欒]' -- the classic UTF-8-read-as-GBK
# signature), so the P4 regex '写字节:\s*([\d.]+)\s*GB' would never match and
# dWrite would silently be MISSING on every run -- a judge that can only ever
# say "unproven", never PASS or FAIL.  Pinning this to UTF-8 here makes PS
# decode native stdout correctly; verified: after the switch the same
# invocation returns '[错误] 权限不足...' with 权限 matching again.
# rbfeatctl's labels are pure ASCII, so it is immune either way.
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# Always keep a durable log.  If the caller did not name one, pick a stable path
# derived from the target dir so that parent and elevated child agree on it and
# the evidence survives both a crash and a closed window.
#
# WHY THIS NO LONGER DEFAULTS TO %TEMP% (changed 2026-11-20, after losing the
# only real-run log).  %TEMP% is globally writable and gets swept by cleanup
# tools, Storage Sense, and -- as happened on 2026-11-20 -- by any test harness
# that clears a glob to make "newest file" deterministic.  The 15:18:40 real-run
# log lived there and was destroyed; it was the single record of the only
# elevated execution this project had, and it cannot be re-created.  A durable
# log that lives in the one directory everyone is expected to clean is not
# durable.  deliverables/design/logs/ is inside the repo: it shows up in git
# status, it is not swept, and anyone deleting it is deleting something they can
# see.  $LogPath remains overridable for callers that have a reason.
if (-not $LogPath) {
    $logDir = Join-Path $PSScriptRoot 'design/logs'
    if (-not (Test-Path -LiteralPath $logDir)) {
        try { New-Item -ItemType Directory -Path $logDir -Force -ErrorAction Stop | Out-Null }
        catch { $logDir = $PSScriptRoot }    # fall back next to the script, never %TEMP%
    }
    $LogPath = Join-Path $logDir ('RB-write-load_{0}.log' -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
}

# ------------------------------------------------------- earliest failure path
# ORDERING BUG THIS FIXES: the red-line assertions below run BEFORE `Say` and
# `Write-LogLine` are defined and BEFORE the main `trap` is registered (a trap
# only applies once its statement has executed).  So a red-line rejection --
# the one failure that must NEVER be silent -- used to die with
# "Say is not recognized" and an EMPTY log.  Verified on 2026-11-20 with
# -TargetDir 'D:\tptest': the guard threw, the handler could not report it,
# exit code came back blank instead of 4.
# Fail-Early uses nothing but built-ins and the already-final $LogPath.
function Fail-Early([string]$message) {
    $stamp = Get-Date -Format 'HH:mm:ss'
    $full = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    Write-Host "  [ABORT] $message" -ForegroundColor Red
    if ($LogPath) {
        try {
            Add-Content -LiteralPath $LogPath -Encoding UTF8 -ErrorAction Stop -Value @(
                ("{0}   [ABORT] {1}" -f $stamp, $message),
                ("RUNABORTED {0} :: {1}" -f $full, $message)
            )
        } catch { }
    }
    exit 4
}

# --------------------------------------------------------------- red lines
# Order matters: check the sacred directory FIRST so the error message is the
# accurate one even if a caller passes something like 'D:\大模型\x'.
# Any rejection here goes through Fail-Early so it lands in the durable log and
# returns rc=4 rather than an unexplained blank exit.
try {
if ($TargetDir -like 'D:\大模型*') {
    throw "RED LINE: refusing to operate on D:\大模型 (user model files)"
}
$qual = (Split-Path -Qualifier $TargetDir) + '\'
if ($qual -eq $TargetDir) {
    throw "RED LINE: TargetDir '$TargetDir' is a volume root, refusing"
}
if ((Split-Path -Leaf $TargetDir) -eq '') {
    throw "RED LINE: TargetDir '$TargetDir' has no leaf directory name"
}
$canonTarget = [IO.Path]::GetFullPath($TargetDir).TrimEnd('\')
if ($canonTarget -ne 'D:\rb-write-test') {
    throw "RED LINE: TargetDir must be exactly D:\rb-write-test, got '$canonTarget'"
}
if ($canonTarget -like 'D:\大模型*') {
    throw "RED LINE: canonical path resolves into D:\大模型"
}
} catch {
    Fail-Early $_.Exception.Message
}

# ------------------------------------------------------------ logging / UI
$script:log = New-Object System.Collections.Generic.List[string]

# DURABLE LOG.  This script runs inside an elevated *separate window*, so the
# parent cannot capture its stdout -- and the failure mode we are hunting is a
# bugcheck, which kills the window outright.  An in-memory-only log would mean
# losing every per-round line at exactly the moment they matter.  So each line
# is appended to disk immediately (Add-Content opens/closes the handle, so it
# survives a process kill).  Truncated once at start of the real executor.
# Truncation policy: the UNELEVATED parent truncates once (it is the first to
# run and writes the PLAN lines); the elevated child (-NoElevate) APPENDS so the
# parent's PLAN lines are preserved and the file ends up as one continuous run.
if ($LogPath) {
    $hdr = "# RB-write-load durable log  start={0}  pid={1}  elevated-session={2}" -f `
        (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $PID, [bool]$NoElevate
    try {
        if ($NoElevate) { Add-Content -LiteralPath $LogPath -Value $hdr -Encoding UTF8 -ErrorAction Stop }
        else { Set-Content -LiteralPath $LogPath -Value $hdr -Encoding UTF8 -ErrorAction Stop }
    } catch { }
    # ---- PROVE THE LOG IS ON DISK, do not assume it -------------------------
    # "the evidence survived" used to be an assumption; on 2026-11-20 the file it
    # referred to no longer existed.  So read it back immediately and print the
    # byte count and SHA256.  If the log cannot be read back, say so loudly and
    # treat the run as evidence-less rather than silently proceeding -- a run
    # whose durable log is missing produces numbers nobody can re-check.
    try {
        $leaf = [IO.File]::ReadAllBytes($LogPath)
        $sha = (Get-FileHash -LiteralPath $LogPath -Algorithm SHA256).Hash
        Write-Host ('  [ok] durable log on disk: {0} B  sha256={1}' -f $leaf.LongLength, $sha)
        Write-Host ('       path: {0}' -f $LogPath)
    } catch {
        Write-Host ('  [FAIL] durable log NOT readable after write: {0}' -f $LogPath) -ForegroundColor Red
        Write-Host '         this run would leave no evidence; refusing to continue silently' -ForegroundColor Red
    }
}
function Write-LogLine([string]$line) {
    if (-not $LogPath) { return }
    try { Add-Content -LiteralPath $LogPath -Value $line -Encoding UTF8 -ErrorAction Stop } catch { }
}
function Say([string]$m, [string]$tag = '') {
    $line = if ($tag) { "  [$tag] $m" } else { "  $m" }
    Write-Host $line
    $stamped = '{0} {1}' -f (Get-Date -Format 'HH:mm:ss'), $line
    $null = $script:log.Add($stamped)
    Write-LogLine $stamped
}
function Head([string]$m) {
    Write-Host ""
    Write-Host "=== $m ===" -ForegroundColor Cyan
    $null = $script:log.Add(('=== {0} ===' -f $m))
    Write-LogLine ('=== {0} ===' -f $m)
}

# ---------------------------------------------------------------- elevation
function Test-Elevated {
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { return $false }
}

# ------------------------------------------------------- dump baseline (R7)
# The lead's correction: never infer crash time from a .dmp filename
# ("112026-48453-01.dmp"'s "48" is not 09:42).  Take ONE full snapshot before
# the load and compare ONCE at the end.  A changed LastWriteTime also counts,
# not just a new name.
$dumpDir = 'C:\Windows\Minidump'
function Get-DumpSnapshot {
    if (-not (Test-Path $dumpDir)) { return @{} }
    $h = @{}
    foreach ($f in Get-ChildItem $dumpDir -Filter *.dmp -ErrorAction SilentlyContinue) {
        $h[$f.Name] = $f.LastWriteTime
    }
    return $h
}
function Compare-Dumps($baseline) {
    $now = Get-DumpSnapshot
    $added = @(); $changed = @(); $removed = @()
    foreach ($k in $now.Keys) {
        if (-not $baseline.ContainsKey($k)) { $added += $k }
        elseif ($baseline[$k] -ne $now[$k]) { $changed += $k }
    }
    foreach ($k in $baseline.Keys) { if (-not $now.ContainsKey($k)) { $removed += $k } }
    return [pscustomobject]@{ Added = $added; Changed = $changed; Removed = $removed; Now = $now }
}
function Show-DumpBaseline($snap) {
    Say "minidump baseline: $($snap.Count) file(s)"
    foreach ($k in ($snap.Keys | Sort-Object)) {
        Say ("  {0}  {1}" -f $k, $snap[$k].ToString('yyyy-MM-dd HH:mm:ss'))
    }
}

# ------------------------------------------------------------- volume space
# Get-PSDrive / DriveInfo work unelevated; `fsutil volume diskfree` needs admin.
function Get-FreeBytes {
    try {
        $di = New-Object System.IO.DriveInfo 'D'
        return [int64]$di.TotalFreeSpace
    } catch { return [int64]0 }
}

# ----------------------------------------------------------------- ctl calls
$repoRoot = 'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包'
$ctl  = Join-Path $repoRoot 'src\ctl\ramboostctl.exe'
$feat = Join-Path $repoRoot 'src\ctl\rbfeatctl.exe'
$verifier = Join-Path $repoRoot 'deliverables\RB-postreboot-verify.ps1'

function Invoke-Ctl([string]$exe, [string[]]$ctlArgs, [string]$label) {
    if (-not (Test-Path -LiteralPath $exe)) { Say "$label : binary missing ($exe)" 'SKIP'; return $null }
    try {
        $out = & $exe @ctlArgs 2>&1
        $rc = $LASTEXITCODE
        Say "$label : rc=$rc"
        foreach ($l in $out) { Say "  $l" }
        if ($rc -eq 1 -and ($out -join ' ') -match '0x00000005|权限不足|Access is denied') {
            Say "$label : NOT ELEVATED -> counter evidence unavailable" 'WARN'
        }
        return ($out -join "`n")
    } catch {
        Say "$label : $($_.Exception.Message)" 'WARN'
        return $null
    }
}

# ----------------------------------------------- P4 counter parsing (deltas)
# P4 must prove THIS load crossed the filter, not merely that some background IO
# moved a counter. Anything that "advances" therefore has to be a byte DELTA
# attributed to one round. Parsing failure records MISSING (never 0): a counter
# we could not read is not a counter that read zero - that is the same
# empty-match-passes trap the repo has hit twice.
function Get-CounterSnapshot([string]$statusText) {
    # Fields that `ramboostctl status` ACTUALLY prints (ramboostctl.c:96-100).
    # Verified against source because an earlier version of this function looked
    # for '读字节: X GB', which the tool NEVER prints -- it prints
    #   '读请求: N   读命中(次数): X%   读命中(字节): Y%'
    # i.e. read counters are a request COUNT and two PERCENTAGES, not a GB value.
    # That regex could therefore never match, so dRead was silently MISSING on
    # every run. It did not do harm (dRead never gates anything) but it made the
    # report look like an instrument fault instead of a wrong assumption.
    # ReadBytes is now derived from the BYTE-HIT PERCENTAGE, which is a ratio and
    # so cannot be inverted to an absolute byte count. Kept as $null deliberately,
    # with ReadSource recording why -- absent stays absent, never faked to 0.
    $snap = [ordered]@{ WriteBytes = $null; WriteBytesLine = $null
                        ReadBytes = $null
                        WriteReq = $null; ReadReq = $null
                        ReadSource = 'unavailable'; Raw = $null }
    if (-not $statusText) { return [pscustomobject]$snap }
    $snap.Raw = $statusText
    # ---- match PER LINE, never across the whole blob -------------------------
    # ramboostctl.c:96-100 emits records terminated with a bare '\n'. A Windows
    # console treats only '\r\n' as a carriage return, so depending on how the
    # text travelled (piped, re-read from a file, captured by Out-String) the
    # same field can arrive with '\n' or '\r\n'. A whole-blob regex still
    # matched, but only because the pattern's '\s*' happened to swallow the
    # '\r' -- the match was accidental, and any future edit that tightened
    # '\s*' to ' ' would break it silently and make dWrite MISSING on every
    # run. Splitting first anchors every pattern to one line, which is also the
    # shape the C code actually emits: one record per line.
    # NOT fixed in ramboostctl.c on purpose: a bare '\n' is the CORRECT byte
    # for the CLI when stdout is redirected to a file, which is a supported use.
    $lines = @($statusText -split "\r?\n")
    foreach ($ln in $lines) {
        # First occurrence wins, matching the old single-pass Match semantics.
        # '写请求: N   写字节: X.XX GB   缓存吸收: Y.YY%'
        if ($null -eq $snap.WriteBytes) {
            $mw = [regex]::Match($ln, '写字节\s*:\s*([0-9]+(?:\.[0-9]+)?)\s*GB')
            if ($mw.Success) {
                $snap.WriteBytes = [decimal]$mw.Groups[1].Value * 1GB
                $snap.WriteBytesLine = $ln.Trim()   # kept as evidence in the report
            }
        }
        # '写请求: N' -- a count, used only for the record (proves the request
        # counter moved in step with the byte counter).
        if ($null -eq $snap.WriteReq) {
            $mq = [regex]::Match($ln, '写请求\s*:\s*([0-9]+)')
            if ($mq.Success) { $snap.WriteReq = [int64]$mq.Groups[1].Value }
        }
        if ($null -eq $snap.ReadReq) {
            $mqr = [regex]::Match($ln, '读请求\s*:\s*([0-9]+)')
            if ($mqr.Success) { $snap.ReadReq = [int64]$mqr.Groups[1].Value }
        }
    }
    $snap.ReadSource = 'ReadBytes not printed by ramboostctl status (it prints read REQUEST count and hit PERCENTAGES, never read GB)'
    return [pscustomobject]$snap
}
function Format-CounterDelta($before, $after, [string]$name) {
    if ($null -eq $before -or $null -eq $after -or $null -eq $before.$name -or $null -eq $after.$name) {
        return 'MISSING'
    }
    $d = $after.$name - $before.$name
    if ($d -lt 0) { return 'MISSING' }   # counter went backwards -> unreadable window
    return ([double]$d)
}
function Get-P4RatioState {
    # P4's judge, extracted as a PURE FUNCTION so it can be mutated and proven.
    #
    # Parameters are [object] ON PURPOSE. Typing $dWriteBytes as [double] makes
    # PowerShell coerce $null to 0.0 at the call boundary, so the "counter
    # unreadable" case would arrive as "counter read zero" and be judged FAIL --
    # absent silently becoming a number, which is the exact defect family this
    # suite was built to catch. Caught by _p4-ratio-selftest case "dWrite null".
    param([object]$dWriteBytes, [object]$expectedBytes)

    if ($null -eq $dWriteBytes -or $null -eq $expectedBytes) { return 'MISSING' }
    $dW = [double]$dWriteBytes
    $ex = [double]$expectedBytes
    if ($ex -le 0) { return 'MISSING' }            # no denominator -> not judgeable
    #
    # WHY BOTH-SIDED. An earlier version only required ratio >= 0.9, and the first
    # real run measured 3,655.68 MB against a 1,800 MB denominator -- ratio 2.03 --
    # and PASSED. That is the same "one-sided assumption, one-sided judge, 100%
    # deviation slips through" error we already made once on gate 2. The write
    # bytes counted here can ONLY come from this load: nothing else on a quiet box
    # writes multiple GB in ten seconds. So there is no physical source for a
    # legitimate 2x overshoot, and the judge must be two-sided.
    #
    # BANDS (the upper bound is derived, not picked):
    #   The judge's denominator is $expectedBytes = 3,600 MiB for a full 2-round
    #   run (1,800 direct + 1,800 copy), so the 1.15 ceiling ALLOWS 540 MiB of
    #   excess. Measured on this box: dWrite = 3,655.68 MiB against 3,600 MiB
    #   requested = +55.68 MiB = +1.5467%, i.e. 27.84 MiB per round.
    #   HEADROOM: 540 MiB / 55.68 MiB = 9.7x, ONE order of magnitude -- not the
    #   "two orders" an earlier draft claimed. Against the rounding noise of this
    #   instrument (worst-case dWrite error +-10.24 MiB, derived below) the margin
    #   is 52.7x. Both ratios are stated so the next reader does not have to
    #   trust an adjective.
    #   WHAT THE 55.68 MiB IS: NOT established. NTFS metadata for two 900 MiB
    #   files has an upper bound of roughly 2-6% of that figure, not 1-30x of it:
    #   $Bitmap at 4 KiB clusters ~= 28 KB/file, one $MFT record is KB-scale, even
    #   a pathologically fragmented 4 KiB layout is ~5.6 MiB/file, and a single
    #   $LogFile checkpoint is <= 64 MiB. So the observed 55.68 MiB is 10-30x
    #   LARGER than the metadata it used to be attributed to. Attributing all of
    #   it to NTFS metadata is an assertion this measurement did NOT isolate;
    #   background writes, checkpoint timing and other processes writing inside
    #   the pre->post window are all still live candidates.
    #   THEREFORE: 1.15 is NOT a physically derived bound. Its job is narrower and
    #   it is kept for that job -- it is a WIDE band that catches the 2x scope
    #   error (1,800 vs 3,600 denominator) and the 0.5 half-load case, while
    #   staying far enough from 1.0 that instrument noise cannot move a healthy
    #   run across it.
    #   HOW MUCH OF THE 1.5467% IS ROUNDING? One printed step is 0.01 GiB =
    #   10.24 MiB and each reading is rounded to a half step, so the true dWrite
    #   lies in [3,645.44 .. 3,665.92] MiB => ratio in [101.2622% .. 101.8311%],
    #   excess in [45.44 .. 65.92] MiB. Signal 1.5467% vs noise +-0.2844
    #   percentage points = signal-to-noise 5.4:1. The 1.55% is therefore not a
    #   rounding artefact -- but it is also not precise enough to support a
    #   per-round "27.84 MiB of metadata" claim, which is exactly why the
    #   attribution above is refused.
    # EXACT 0.5 and EXACT 2.0 are FAIL, not WARN, and that is deliberate:
    #   * exactly half means one of the two contributions is missing entirely
    #     (one round absent, or the copy writes never counted) -- structural, not
    #     noise;
    #   * exactly double is the signature of a DOUBLE COUNT (both sources counted
    #     twice, or the denominator halved).
    #   A coincidental landing on either exact value is far less likely than one
    #   of those two causes, so the band is inclusive-FAIL at both ends. This also
    #   matches the required mutation case M2 (1,800 / 3,600 -> FAIL).
    $ratio = $dW / $ex
    if ($ratio -le 0) { return 'FAIL' }            # dWrite = 0: nothing crossed
    if ($ratio -le 0.5) { return 'FAIL' }          # half or less: a whole source is missing
    if ($ratio -lt 0.90) { return 'WARN-LOW' }     # below the model, not absurd
    if ($ratio -le 1.15) { return 'PASS' }         # within model + NTFS metadata
    if ($ratio -lt 2.0)  { return 'WARN-HIGH' }    # above model, below absurd
    return 'FAIL'                                  # >= 2x: scope or counter is wrong
}
function Format-P4RatioNote([object]$dWriteBytes, [object]$expectedBytes, [string]$state) {
    $dW = if ($null -eq $dWriteBytes) { 0.0 } else { [double]$dWriteBytes }
    $ex = if ($null -eq $expectedBytes -or [double]$expectedBytes -le 0) { 0.0 } else { [double]$expectedBytes }
    $ratio = if ($ex -gt 0) { $dW / $ex } else { 0 }
    $dGB = $dW / 1GB; $eGB = $ex / 1GB
    switch ($state) {
        'PASS'      { return ('dWrite={0:N2} GB vs expected {1:N2} GB (ratio {2:P2}) -> inside [0.90, 1.15]' -f $dGB, $eGB, $ratio) }
        'WARN-LOW'  { return ('dWrite={0:N2} GB vs expected {1:N2} GB (ratio {2:P2}) -> BELOW 0.90: part of the load did not cross the filter' -f $dGB, $eGB, $ratio) }
        'WARN-HIGH' { return ('dWrite={0:N2} GB vs expected {1:N2} GB (ratio {2:P2}) -> ABOVE 1.15, below 2.0: more bytes than our writes alone explain; check for other writers before accepting' -f $dGB, $eGB, $ratio) }
        'FAIL'      { return ('dWrite={0:N2} GB vs expected {1:N2} GB (ratio {2:P2}) -> outside (0.5, 2.0): the numerator and denominator are not measuring the same thing, or the counter is wrong' -f $dGB, $eGB, $ratio) }
        default     { return ('dWrite={0:N2} GB vs expected {1:N2} GB (ratio {2:P2}) -> MISSING: counter unreadable (never treated as 0)' -f $dGB, $eGB, $ratio) }
    }
}
function Get-P0State {
    # P0's state machine, extracted as a PURE FUNCTION for the same reason
    # Get-P4RatioState was: so the replay harness can slice the SHIPPED code
    # instead of re-implementing it. Before this extraction the state machine was
    # inlined in the main body, which meant _replay-real-run.ps1 had to carry its
    # own rewritten copy -- proving the INTENT of the fix, never the shipped
    # bytes. That is the "verify the copy, not the artifact" failure this suite
    # exists to catch, found in the harness of the very file that documents it.
    #
    # Semantics are unchanged from the inline version; every branch below is the
    # same predicate in the same order. The caller still owns the printing, so the
    # operator-facing text did not move.
    #
    # Parameters are [object] ON PURPOSE (same trap as Get-P4RatioState): a
    # [double] parameter would coerce $null to 0.0 at the call boundary and turn
    # "counter unreadable" into "counter read zero", i.e. absent becoming a
    # number. Anything unreadable must stay 'MISSING' and reach UNREADABLE.
    param(
        [object]$dMisses,        # [int64] or $null
        [object]$dReadReq,       # [int64] or $null
        [object]$l2Before,       # L2state of the first sample
        [object]$l2After,        # L2state of the second sample
        [object]$gate            # the 2x anchor gate
    )

    # Unreadable samples are their own state. A counter we could not read is not a
    # counter that read a small number; folding them together is the "empty set
    # passes" family that lets a broken instrument look normal.
    if ($null -eq $dMisses -or $null -eq $gate) { return 'UNREADABLE' }
    # L2 on means RbFeatOnMiss may skip the disk read entirely (rbfeat.c:921-922 +
    # rbio_worker.c:257/:290), so dMisses stops meaning "disk I/O count". A reading
    # that cannot mean what this control needs it to mean is UNREADABLE.
    if ($null -eq $l2Before -or $null -eq $l2After) { return 'UNREADABLE' }
    if (([int]$l2Before -ne 0) -or ([int]$l2After -ne 0)) { return 'UNREADABLE' }

    $dM = [int64]$dMisses
    $g  = [int64]$gate
    # The anchor's premise being OBSERVED is what separates an informative zero
    # from a meaningless one. The caller passes the STRING 'MISSING' for an
    # unreadable read-request counter (it computes $dReadReq as either a number or
    # that sentinel), so this must accept a number, $null, or 'MISSING' -- casting
    # 'MISSING' to [int64] throws, and a throw here would abort the whole P0 block
    # and be reported as a harness fault instead of an uninformative zero. Caught
    # by selftest case ZERONOREQ, which is exactly the case that feeds it.
    $readReqKnown = ($null -ne $dReadReq) -and ("$dReadReq" -ne 'MISSING') -and ("$dReadReq" -ne '')
    $copyReadObserved = $readReqKnown -and ([int64]$dReadReq -gt 0)

    if ($dM -eq 0 -and $copyReadObserved) { return 'ZERO-AS-EXPECTED' }
    if ($dM -eq 0)                        { return 'ZERO-UNINFORMATIVE' }
    if ($dM -le $g)                       { return 'LOW-AS-EXPECTED' }
    return 'HIGH-UNEXPECTED'
}
function Write-RealFile([string]$path, [int]$sizeMB, [int]$seed) {
    $chunkMB = 8
    $chunk = New-Object byte[] ($chunkMB * 1MB)
    (New-Object Random $seed).NextBytes($chunk)
    $total = [int64]$sizeMB * 1MB
    $written = [int64]0
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $fs = [IO.File]::Create($path, $total, [IO.FileOptions]::None)
    try {
        while ($written -lt $total) {
            $n = [Math]::Min($chunk.Length, $total - $written)
            $fs.Write($chunk, 0, [int]$n)
            $written += $n
            # Push dirty pages down every 16 MB; otherwise the whole 900 MB sits
            # in the page cache and never crosses the filter.
            if (($written % (16MB)) -lt $chunk.Length) { $fs.Flush($true) }
        }
        $fs.Flush($true)
    } finally { $fs.Dispose() }
    $sw.Stop()
    $sec = [Math]::Max($sw.Elapsed.TotalSeconds, 0.001)
    $rate = $sizeMB / $sec
    Say ("write {0} MB in {1:N2}s -> {2:N1} MB/s" -f $sizeMB, $sec, $rate) 'ok'
    return [pscustomobject]@{ Seconds = $sec; MBps = $rate }
}

function Get-VdlBytes([string]$path) {
    $out = & fsutil file queryvaliddata $path 2>&1
    $line = ($out | Out-String).Trim()
    if ($line -match '\((\d+)\)') { return [int64]$Matches[1] }
    return [int64]-1
}

function Assert-Vdl([string]$path, [int]$expectMB) {
    $want = [int64]$expectMB * 1MB
    $got = Get-VdlBytes $path
    if ($got -eq $want) {
        Say ("VDL = {0} = {1} MB (fully written)" -f $got, $expectMB) 'ok'
        return $true
    }
    Say ("VDL = {0} but expected {1} -> not fully written, round VOID" -f $got, $want) 'FAIL'
    return $false
}

# Whole-volume flush.  `fsutil volume flush` is the stronger primitive but needs
# admin; Write-VolumeCache works unelevated yet proves little.  We already are
# elevated here, so prefer fsutil and record exactly what happened.
function Flush-WholeVolume {
    $out = & fsutil volume flush D: 2>&1
    $rc = $LASTEXITCODE
    if ($rc -eq 0) {
        Say "fsutil volume flush D: -> OK (metadata pushed)" 'ok'
        return $true
    }
    Say "fsutil volume flush D: -> rc=$rc : $($out -join ' ')" 'FAIL'
    try {
        Write-VolumeCache -DriveLetter D -ErrorAction Stop | Out-Null
        Say "fallback Write-VolumeCache -DriveLetter D -> issued (no error surfaced)" 'WARN'
        Say "  NOTE: this cmdlet cannot prove metadata reached the disk." 'WARN'
    } catch {
        Say "fallback Write-VolumeCache also failed: $($_.Exception.Message)" 'FAIL'
    }
    return $false
}

# =================================================================== dry run
$isAdmin = Test-Elevated
$t0All = Get-Date
Head "PLAN (dry run=$([bool]$DryRun), elevated=$isAdmin)"
Say "target dir  : $canonTarget"
Say "rounds      : $Rounds"
Say "size/round  : $SizeMB MB x2 files (A+B, peak $($SizeMB*2) MB on disk; each asserted via VDL)"
Say "peak on disk: ~$([math]::Round($SizeMB*2/1024.0,1)) GB   D: free = $([math]::Round((Get-FreeBytes)/1GB,1)) GB"
Say "wall clock  : $($t0All.ToString('yyyy-MM-dd HH:mm:ss'))"
$snap0 = Get-DumpSnapshot
Show-DumpBaseline $snap0
Say ""
Say "commands that WILL run (in this exact order):" 'plan'
Say "  0) elevate once: Start-Process -Verb RunAs (only if not already elevated)" 'plan'
Say "  1) baseline    : RB-postreboot-verify.ps1   (inside the same elevated session)" 'plan'
Say "  2) pre         : ramboostctl status ; rbfeatctl vols ; free-space ; dump snapshot" 'plan'
for ($r = 1; $r -le $Rounds; $r++) {
    Say "  R$r S1 : write $SizeMB MB -> $canonTarget\r$r-A.bin  (8 MB chunks, Flush(true) every 16 MB)" 'plan'
    Say "  R$r S2 : fsutil file queryvaliddata  (ASSERT == $SizeMB MB)" 'plan'
    Say "  R$r S3 : Copy-Item A -> B ; Flush(true) on B ; ASSERT VDL(B) == $SizeMB MB" 'plan'
    Say "  R$r S4 : fsutil volume flush D:      (whole-volume, pushes NTFS metadata)" 'plan'
    Say "  R$r S5 : ramboostctl status  (after copy, before delete)" 'plan'
    Say "  R$r S6 : Remove-Item A ; Remove-Item B" 'plan'
    Say "  R$r S7 : ASSERT both paths gone ; ASSERT free space grew ~$($SizeMB*2) MB" 'plan'
    Say "  R$r S8 : ramboostctl status ; rbfeatctl vols" 'plan'
}
Say "  post : ramboostctl status ; rbfeatctl vols ; ramboostctl log" 'plan'
Say "         UpperFilters / FilterArmed / WriteBack / DeploySeq / Features\Flags (read-only)" 'plan'
Say "         dump snapshot compared ONCE against the baseline above" 'plan'
Say ""
Say "clear pass / fail criteria:" 'ok'
Say "  P1 no new minidump AND no dmp LastWriteTime change           -> crash-free" 'ok'
Say "  P2 every write and copy round asserts VDL == size            -> bytes really reached the volume" 'ok'
Say "  P3 free space after each delete grows by >= 50% of deleted   -> clusters really freed" 'ok'
Say "     (>=95% = clean; 50-95% = WARN, NTFS metadata overhead; <50% = FAIL)" 'ok'
Say "  P4 dWrite within [90%, 115%] of the bytes this load caused -> IRP crossed the filter" 'ok'
Say "     expected = direct writes AND copy writes (Copy-Item is not server-side here);" 'ok'
Say "     ratio <50% or >200% = FAIL (scope or counter wrong); 50-90% / 115-200% = WARN;" 'ok'
Say "     dWrite=0 = FAIL; unreadable = MISSING, never 0" 'ok'
Say "  P5 UpperFilters still contains RAMBlockCache; FilterArmed=0; Features\Flags=0; WriteBack=0" 'ok'
Say "  P0 MISS COUNTER READ-PATH-ONLY CONTROL (read-only, same session):" 'ok'
Say "     rbfeatctl stats before/after (non-JSON: L2state only exists there)" 'ok'
Say "     This load only WRITES; RbFeatOnMiss has exactly two call sites in the repo" 'ok'
Say "     (rbio_worker.c:256 / :289) and both are on the READ path. So dMisses here" 'ok'
Say "     should be ~0, and that is itself the check: it proves the miss counter is" 'ok'
Say "     not being fed by the write path." 'ok'
Say "     threshold = copyread/131072, the legitimate READ traffic of this run:" 'ok'
Say "       RbFeatOnMiss adds 1 per call (rbfeat.c:919) and one call covers at most" 'ok'
Say "       RB_READ_PLAN_MAX_BYTES=131072 (read_plan.h:29, compile-time assert), so" 'ok'
Say "       dMisses intervals can only be explained by dMisses*131072 read bytes." 'ok'
Say "       The candidate read traffic we can name in advance is the COPIES, but that" 'ok'
Say "       is an ASSUMPTION and on this box it did NOT hold: 2 x 900 MB = 1,887,436,800 B" 'ok'
Say "       would give ANCHOR = 14,400 intervals, which is therefore an UPPER BOUND," 'ok'
Say "       not an expected value (measured 2026-11-20: read-request delta = 0)." 'ok'
Say "       Our own writes are NOT counted as legitimate read traffic: they went" 'ok'
Say "       through IRP_MJ_WRITE (P4 proves that separately). If write bytes appear" 'ok'
Say "       as read traffic, that is exactly the anomaly this control looks for." 'ok'
Say "     dMisses = 0 and the copies DID read from the volume -> ZERO-AS-EXPECTED" 'ok'
Say "     dMisses = 0 but read requests never moved            -> ZERO-UNINFORMATIVE" 'ok'
Say "       (the control could not exercise its own premise; NOT a pass, NOT a fail;" 'ok'
Say "        measured 2026-11-20: dReadReq=0 across the whole load, so the copies were" 'ok'
Say "        served from cache - see the anchor note below)" 'ok'
Say "     0 < dMisses <= 28,800      -> LOW-AS-EXPECTED  (copies + metadata)" 'ok'
Say "     dMisses > 28,800           -> HIGH-UNEXPECTED (WARN only, never gates rc)" 'ok'
Say "     any counter unreadable, or L2state!=0 -> UNREADABLE (NOT a pass; the" 'ok'
Say "       reading cannot mean what this control needs it to mean)" 'ok'
Say "     NTFS metadata reads for one 900 MB file are only a few MB (~1,024..8,192" 'ok'
Say "     intervals at 4 KiB), well inside the gate, so ordinary metadata will not" 'ok'
Say "     trip this; a read of one whole file would approach it." 'ok'
Say "     NO D-01 CONCLUSION is drawn here (that needs the bench read path, separate" 'ok'
Say "     script). P0 never changes rc; it is reported, not gated." 'ok'
Say ""
Say "what a PASS will NOT prove:" 'warn'
Say "  * NOT '0x7F is fixed' - only 'not reproduced' under one load shape on one box" 'warn'
Say "  * NOT that paging-IO writes or every IRP_MJ_SET_INFORMATION sub-op was covered" 'warn'
Say "  * NOT a substitute for full Driver Verifier (only RAMBlockCache.sys, 0x1209BB)" 'warn'

if ($DryRun) {
    Say ""
    Say "DRY RUN: nothing written. Re-run without -DryRun to execute." 'DRYRUN'
    Write-LogLine ("DRYRUN-END {0}" -f (Get-Date -Format 'HH:mm:ss'))
    Write-Host ""
    Write-Host "---- DRY RUN OUTPUT BELOW (paste-back friendly) ----"
    $script:log | ForEach-Object { Write-Host $_ }
    Write-Host ""
    Write-Host "durable log: $LogPath"
    exit 0
}

# =================================================================== elevate
if (-not $isAdmin -and -not $NoElevate) {
    Head "ELEVATING (single UAC)"
    $self = $PSCommandPath
    $argList = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$self`"",
        '-Rounds', $Rounds, '-SizeMB', $SizeMB, '-TargetDir', "`"$canonTarget`"",
        '-NoElevate'
    )
    if ($SkipVerifier) { $argList += '-SkipVerifier' }
    if ($LogPath) { $argList += @('-LogPath', "`"$LogPath`"") }
    # Repo discipline: always PowerShell 7 when present. The elevated child is the
    # one that runs [IO.File]::Open + Flush($true), parses fsutil output and does
    # [math]::Round - all of which differ on PS 5.1 (native stdout encoding,
    # EAP='Stop' vs native stderr). The non-elevated self-test never covered this
    # branch, so the interpreter must be pinned here, not inherited.
    $childExe = 'C:\Program Files\PowerShell\7\pwsh.exe'
    $childIsPwsh7 = Test-Path -LiteralPath $childExe
    if (-not $childIsPwsh7) {
        $childExe = 'powershell.exe'
        Say "pwsh 7 NOT found - falling back to Windows PowerShell 5.1 for the elevated child" 'WARN'
    }
    Say "re-launching elevated: $childExe $($argList -join ' ')"
    Write-LogLine ("interpreter: {0} (pwsh7={1})" -f $childExe, $childIsPwsh7)
    Start-Process -FilePath $childExe -Verb RunAs -ArgumentList $argList

    # RESULT MUST COME BACK.  Start-Process -Verb RunAs cannot return the child's
    # exit code, and a bare `exit 0` would report "UAC cancelled" and "all checks
    # passed" identically - the "$LASTEXITCODE inherited -> fake success" class
    # this project keeps re-hitting.  So the parent tails the durable log for the
    # machine-readable TERMINAL line and propagates its rc.  Meanwhile the child
    # writes every line to disk immediately, so a bugcheck cannot erase evidence.
    Say "waiting for the elevated child to report TERMINAL (up to 20 min) ..."
    $deadline = (Get-Date).AddMinutes(20)
    $sawChild = $false
    $terminal = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        if ($LogPath -and (Test-Path -LiteralPath $LogPath)) {
            if (-not $sawChild -and (Select-String -LiteralPath $LogPath -Pattern 'elevated-session=True' -Quiet)) {
                $sawChild = $true
                Say "elevated child is alive, durable log live: $LogPath" 'ok'
            }
            $hit = Select-String -LiteralPath $LogPath -Pattern '^TERMINAL ' | Select-Object -Last 1
            if (-not $hit) { $hit = Select-String -LiteralPath $LogPath -Pattern '^RUNABORTED ' | Select-Object -Last 1 }
            if ($hit) { $terminal = $hit.Line; break }
        }
    }
    if ($terminal) {
        Say "child reported: $terminal"
        # rc extraction order matters:
        #   TERMINAL   carries an explicit rc= -> propagate it verbatim.
        #   RUNABORTED carries NO rc by design (it is written from Fail-Early and
        #     from the trap, which both exit 4).  Propagating that as 3 would
        #     collapse "a red line was hit" (4) into "a verdict failed" (3), and
        #     those must never be the same code: one means we refused to touch
        #     user data, the other means we touched it and the load failed.
        if ($terminal -match 'rc=(\d+)') { exit [int]$Matches[1] }
        if ($terminal -match '^RUNABORTED ') { exit 4 }
        exit 3
    }
    Say "child produced no TERMINAL/RUNABORTED line within 20 min - inspect the durable log" 'FAIL'
    exit 3
}

# ================================================================== execute
if (-not $isAdmin) {
    Say "still not elevated and -NoElevate was set -- counter evidence will be missing." 'WARN'
}

# Any uncaught throw inside the executor used to exit silently with no trace in
# the durable log (the window closes and all we would see is a truncated file).
# This trap makes "the run died for a reason the script knows about" explicit
# and distinguishable from "the machine bugchecked".
trap {
    $msg = $_.Exception.Message
    Say "UNCAUGHT: $msg" 'ABORT'
    Write-LogLine ("RUNABORTED {0} :: {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg)
    Write-LogLine "  at: $($_.InvocationInfo.PositionMessage -replace "`r?`n", ' | ')"
    exit 4
}

if (-not (Test-Path $canonTarget)) { New-Item -ItemType Directory -Path $canonTarget -Force | Out-Null }
Say "target dir ready: $canonTarget"

$free0 = Get-FreeBytes
$needBytes = [int64]$SizeMB * 2 * 1MB
Say "free before: $([math]::Round($free0/1GB,2)) GB ; need $([math]::Round($needBytes/1GB,2)) GB"
if ($free0 -lt ($needBytes + 2GB)) { throw "not enough free space on D:" }

# ---- baseline verifier (same elevated session, no second UAC) --------------
if ($SkipVerifier) {
    Say "verifier skipped by request (-SkipVerifier)" 'SKIP'
} elseif (Test-Path -LiteralPath $verifier) {
    Head "BASELINE VERIFIER (RB-postreboot-verify.ps1)"
    try {
        $vout = & powershell -NoProfile -ExecutionPolicy Bypass -File $verifier 2>&1
        $vrc = $LASTEXITCODE
        foreach ($l in $vout) { Say "  $l" }
        Say "verifier rc=$vrc (0 = all hard checks passed)" $(if ($vrc -eq 0) { 'ok' } else { 'WARN' })
    } catch {
        Say "verifier threw: $($_.Exception.Message)" 'WARN'
    }
} else {
    Say "verifier missing: $verifier" 'SKIP'
}

Head "PRE"
$preStatusText = Invoke-Ctl $ctl @('status') 'pre status'
$snapPre = Get-CounterSnapshot $preStatusText
Say ("pre counters: writeBytes={0} readBytes={1}" -f `
        $(if ($null -eq $snapPre.WriteBytes) { 'MISSING' } else { '{0:N2} GB' -f ($snapPre.WriteBytes / 1GB) }), `
        $(if ($null -eq $snapPre.ReadBytes)  { 'MISSING' } else { '{0:N2} GB' -f ($snapPre.ReadBytes  / 1GB) }))
Invoke-Ctl $feat @('vols') 'pre vols' | Out-Null
Show-DumpBaseline $snap0

# ---- P0: MISS COUNTER READ-PATH-ONLY CONTROL -------------------------------
# Read-only. Uses the SAME elevated session so it costs no extra UAC.
# This is a CONTROL on the miss counter, NOT a D-01 judge -- the D-01 verdict
# needs the bench read path and lives in RB-bench-miss-coalescing.ps1.
# RbFeatOnMiss counts unconditionally (rbfeat.c:919, before the g_Rb2Flags gate),
# so this works with Flags=0.
#
# FIELD SOURCES -- both branches print miss, so the choice is about SAMPLING
# INSTANTS, not availability. Verified against the printf lines (not memory; the
# lead and I both mis-stated this once):
#   rbfeatctl.c:604  printf("hits=%llu miss=%llu bytesHit=%.1fMB bytesMiss=%.1fMB\n", ...)
#   rbfeatctl.c:615  printf("L2state=%lu thermalStops=%lu admitRejects=%lu\n", ...)
#   rbfeatctl.c:592/597  JSON: "miss":%llu and "bm":%llu (raw bytes, no rounding)
#
#   miss      -- BOTH branches have it. JSON is still preferred, because it
#   bm (bytes) - ONLY JSON has raw bytes. Non-JSON's bytesMiss=%.1fMB rounds to
#                0.1 MB, and that rounding ALONE can fail the N2 consistency check
#                at small dMisses: dMisses=1000 -> expected 3.9 MB, and +-0.1 MB of
#                rounding is already 2.6%, over the 2% threshold. A pure write
#                load is exactly the case where dMisses is small, so the rounded
#                field would produce a FALSE FAIL on a clean run. JSON's exact
#                integer removes the whole problem.
#   L2state   -- ONLY non-JSON (rbfeatctl.c:615). Must be a separate call.
#
# So: L2state gate FIRST via non-JSON; if it passes, take miss+bm together in ONE
# JSON call. Two samples of the same counter taken at two different instants
# would drift apart while the driver keeps counting.
function Get-FeatSample([string]$label) {
    $empty = [pscustomobject]@{ Label = $label; Hits = $null; Misses = $null
                                BytesMiss = $null; L2State = $null; Flags = $null
                                Ok = $false; L2Ok = $false; Source = 'none' }
    if (-not (Test-Path -LiteralPath $feat)) {
        Say "$label : rbfeatctl missing -> MISSING" 'WARN'; return $empty
    }

    # --- gate sample: L2state + Flags, from the plain (non-JSON) form only.
    $l2Text = ((& $feat @('stats') 2>&1) | Out-String)
    $l2rc = $LASTEXITCODE
    $s = [pscustomobject]@{ Label = $label; Hits = $null; Misses = $null
                            BytesMiss = $null; L2State = $null; Flags = $null
                            Ok = $false; L2Ok = $false; Source = 'none' }
    if ($l2rc -eq 0 -and $l2Text -match 'L2state=(\d+)') {
        $s.L2State = [int64]$Matches[1]
        $s.L2Ok = $true
    }
    if ($l2Text -match 'flags=0x([0-9A-Fa-f]+)') { $s.Flags = [Convert]::ToInt64($Matches[1], 16) }

    # --- counter sample: miss + bm together, from JSON (exact integers).
    $jText = ((& $feat @('stats', '-json') 2>&1) | Out-String)
    $jrc = $LASTEXITCODE
    if ($jrc -eq 0 -and $jText -match '"miss":(\d+)') { $s.Misses = [int64]$Matches[1] }
    if ($jrc -eq 0 -and $jText -match '"bm":(\d+)')   { $s.BytesMiss = [int64]$Matches[1] }
    if ($jText -match '"hits":(\d+)') { $s.Hits = [int64]$Matches[1] }
    if ($null -ne $s.Misses -and $null -ne $s.BytesMiss) { $s.Source = 'json' }

    # Fall back to the plain form's rounded values ONLY if JSON failed outright,
    # and record that it is rounded so N2 cannot silently trust it.
    if ($null -eq $s.Misses -and $l2Text -match 'miss=(\d+)') { $s.Misses = [int64]$Matches[1]; $s.Source = 'plain-rounded' }
    if ($null -eq $s.BytesMiss -and $l2Text -match 'bytesMiss=([0-9]+(?:\.[0-9]+)?)MB') {
        $s.BytesMiss = [int64]([double]$Matches[1] * 1MB); $s.Source = 'plain-rounded'
    }

    # MISSING discipline: an absent field stays $null, NEVER 0. 0 is a real
    # possible value; "could not read" is not.
    $s.Ok = ($null -ne $s.Misses) -and ($null -ne $s.BytesMiss)
    if (-not $s.Ok) {
        Say ("$label : miss={0} bm={1} L2state={2} -> MISSING (NOT 0)" -f `
            $s.Misses, $s.BytesMiss, $s.L2State) 'WARN'
    } else {
        Say ("$label : miss={0} bm={1} ({2}) L2state={3} flags=0x{4:X}" -f `
            $s.Misses, $s.BytesMiss, $s.Source, $s.L2State, $s.Flags) 'ok'
    }
    return $s
}
Say ""
Head "MISS COUNTER READ-PATH-ONLY CONTROL"
$p0Before = Get-FeatSample 'P0 before'

$crash = $false
$verdicts = New-Object System.Collections.Generic.List[string]
$totalWriteMB = 0; $totalCopyMB = 0; $totalDelMB = 0
$writeRates = New-Object System.Collections.Generic.List[double]
$metaFlushOk = $true
$hiDumpRemoved = 0            # baseline dmps that disappeared -> baseline integrity
$roundP4Notes = New-Object System.Collections.Generic.List[string]
# ONE source of truth for the P4 denominator. Each round appends the number of
# bytes it should have written through IRP_MJ_WRITE (see the round body); the
# global check sums this list instead of recomputing from totals. Two independent
# formulas is exactly how the global check ended up at half the truth while the
# round check was right.
$roundExpectedBytes = New-Object System.Collections.Generic.List[int64]

for ($r = 1; $r -le $Rounds -and -not $crash; $r++) {
    $tRound = Get-Date
    Head "ROUND $r (start $($tRound.ToString('HH:mm:ss')))"
    $a = Join-Path $canonTarget ("r{0}-A.bin" -f $r)
    $b = Join-Path $canonTarget ("r{0}-B.bin" -f $r)

    # Per-round counter baseline, so attribution cannot be diluted by a
    # neighbouring round that did cross the filter.
    $snapR0Text = Invoke-Ctl $ctl @('status') "R$r status at round start"
    $snapR0 = Get-CounterSnapshot $snapR0Text

    # S1 real write
    Say "S1 write $SizeMB MB -> $a"
    $wr = Write-RealFile $a $SizeMB (1000 + $r)
    $writeRates.Add($wr.MBps)
    $totalWriteMB += $SizeMB
    # ---- P4 denominator contribution: appended HERE, not at the end of the round.
    # WHY IT MOVED (option A of the two the review offered): the expected bytes for
    # this round are fully determined at this point -- A has been written and B is
    # about to be produced by Copy-Item, so the round's IRP_MJ_WRITE total is
    # SizeMB*2 whether or not the rest of the round completes. Appending at the end
    # put it AFTER the two `break`s at the VDL asserts, so any early exit skipped
    # the append while $totalWriteMB above had already been incremented -- the
    # denominator then silently under-counted a round and the ratio came out too
    # high. That never reached a false PASS (both those breaks push a 'P2' string
    # into $verdicts, so rc is forced to 3 and P4 never decides anything), but the
    # safety came from a NEIGHBOUR's habit rather than from the data. Appending
    # here makes the denominator's completeness a property of the code path that
    # determined the number, so a future break added above cannot lose it.
    $roundExpectedBytes.Add([int64]($SizeMB * 2 * 1MB))

    # S2 VDL assert
    if (-not (Assert-Vdl $a $SizeMB)) { $verdicts.Add("R$r P2 write VDL FAIL"); break }
    $d1 = Compare-Dumps $snap0
    if ($d1.Added.Count -or $d1.Changed.Count) { $crash = $true; Say "CRASH after write" 'CRASH'; break }

    # S3 copy + per-file flush + VDL assert
    Say "S3 copy -> $b"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Copy-Item -LiteralPath $a -Destination $b -Force
    $fsB = [IO.File]::Open($b, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
    try { $fsB.Flush($true) } finally { $fsB.Dispose() }
    $sw.Stop()
    $copyMBps = $SizeMB / [Math]::Max($sw.Elapsed.TotalSeconds, 0.001)
    Say ("copy+file-flush {0:N2}s -> {1:N0} MB/s" -f $sw.Elapsed.TotalSeconds, $copyMBps) 'ok'
    if (-not (Assert-Vdl $b $SizeMB)) { $verdicts.Add("R$r P2 copy VDL FAIL"); break }
    $totalCopyMB += $SizeMB

    # S4 whole-volume flush (NTFS metadata)
    if (-not (Flush-WholeVolume)) { $metaFlushOk = $false }

    # S5 counters after copy -- snapshot the numeric value for PER-ROUND delta
    # attribution (see verdict).  A global pre->post delta alone could hide
    # "round 1 never crossed the filter but round 2 did".
    $s5Text = Invoke-Ctl $ctl @('status') "R$r status after copy"
    $snapR = Get-CounterSnapshot $s5Text
    # ---- P4 inside the round: SAME formula as the global check --------------
    # The denominator must count EVERY IRP_MJ_WRITE this round causes, and there
    # are two of them per round:
    #   (a) our own create-write of A (size MB), and
    #   (b) the write that Copy-Item performs to produce B.
    # (b) is real filter traffic: the first real run measured a write-request
    # delta of 13,526 for 3,600 MB total, i.e. ~4 requests per 1 MB, and a byte
    # delta of 3,655.68 MB against 3,600 MB requested -- so Copy-Item did NOT do
    # a server-side copy, B really was written through the filter.
    # An earlier version counted only (a) here (SizeMB*2 -- which is A of round r
    # PLUS B of round r, so it happened to be right) while the GLOBAL check counted
    # only $totalWriteMB = direct writes = half the truth. Both are now derived
    # from one place: $roundExpectedBytes, this round's entry in the contribution
    # list below, which the global sum then adds up.
    $dRoundBytes = Format-CounterDelta $snapR0 $snapR 'WriteBytes'
    $expectRoundBytes = [int64]$SizeMB * 2 * 1MB      # A written + B copied (per round)
    # NOTE: this round's contribution was already appended to $roundExpectedBytes
    # right after the write succeeded (see the comment there). No append here --
    # a second one would double-count the round.
    if ($dRoundBytes -eq 'MISSING') {
        Say "R$r P4 counters MISSING -- cannot attribute this round" 'WARN'
        $roundP4Notes.Add("R$r P4 MISSING")
    } else {
        $rrState = Get-P4RatioState ([double]$dRoundBytes) ([double]$expectRoundBytes)
        $rr = [double]$dRoundBytes / $expectRoundBytes
        Say ("R$r P4 dWrite={0:N2} MB vs expected {1:N2} MB (ratio {2:P0})" -f `
                ([double]$dRoundBytes / 1MB), ($expectRoundBytes / 1MB), $rr) `
            $(switch ($rrState) { 'PASS' { 'ok' } 'FAIL' { 'FAIL' } default { 'WARN' } })
        if ($rrState -eq 'FAIL') {
            $verdicts.Add("R$r P4 FAIL: dWrite=$('{0:N0}' -f ([double]$dRoundBytes/1MB)) MB vs expected $('{0:N0}' -f ($expectRoundBytes/1MB)) MB, ratio $('{0:P0}' -f $rr) is outside [0.5, 2.0]")
        } elseif ($rrState -eq 'WARN-LOW') {
            $verdicts.Add("R$r note: P4 partial attribution $('{0:P0}' -f $rr) (<90%)")
        } elseif ($rrState -eq 'WARN-HIGH') {
            $verdicts.Add("R$r note: P4 over-attribution $('{0:P0}' -f $rr) (>115%, <200%) - more bytes crossed than our writes alone explain")
        }
        $roundP4Notes.Add(("R$r dWrite={0:N2} MB / expected {1:N2} MB (ratio {2:P0}) [{3}]" -f `
                ([double]$dRoundBytes / 1MB), ($expectRoundBytes / 1MB), $rr, $rrState))
    }
    $d2 = Compare-Dumps $snap0
    if ($d2.Added.Count -or $d2.Changed.Count) { $crash = $true; Say "CRASH after copy+flush" 'CRASH'; break }

    # S6 delete
    $freeBeforeDel = Get-FreeBytes
    Say "S6 delete A and B"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Remove-Item -LiteralPath $a -Force
    Remove-Item -LiteralPath $b -Force
    $sw.Stop()
    Say ("delete {0} MB in {1:N2}s" -f ($SizeMB * 2), $sw.Elapsed.TotalSeconds) 'ok'
    $totalDelMB += $SizeMB * 2

    # S7 assert really gone + clusters freed
    $gone = (-not (Test-Path -LiteralPath $a)) -and (-not (Test-Path -LiteralPath $b))
    Say "both paths gone = $gone" $(if ($gone) { 'ok' } else { 'FAIL' })
    if (-not $gone) { $verdicts.Add("R$r delete: path still exists") }

    # Cluster reclaim: NTFS frees clusters on delete for these sizes, but $MFT /
    # directory growth and lazy scavenging mean the delta is rarely exactly the
    # deleted size.  Threshold rationale:
    #   >=95%  -> clean
    #   >=50%  -> more than one full 900 MB round's worth came back, which is
    #             what the lead actually asked to confirm (+900 MB); WARN only
    #   <50%   -> clusters did NOT really free, hard fail
    $freeAfterDel = Get-FreeBytes
    $grew = $freeAfterDel - $freeBeforeDel
    $expect = [int64]$SizeMB * 2 * 1MB
    $ratio = if ($expect -gt 0) { $grew / $expect } else { 0 }
    Say ("free after delete: +{0:N0} B = {1:P0} of {2:N0} B" -f $grew, $ratio, $expect) `
        $(if ($ratio -ge 0.95) { 'ok' } elseif ($ratio -ge 0.5) { 'WARN' } else { 'FAIL' })
    if ($ratio -lt 0.5) {
        $verdicts.Add("R$r P3 clusters not freed: only $('{0:P0}' -f $ratio)")
    } elseif ($ratio -lt 0.95) {
        $verdicts.Add("R$r note: reclaim $('{0:P0}' -f $ratio) (<95%, metadata overhead)")
    }

    # S8 counters after delete
    Invoke-Ctl $ctl @('status') "R$r status after delete" | Out-Null
    Invoke-Ctl $feat @('vols') "R$r vols" | Out-Null

    $d3 = Compare-Dumps $snap0
    if ($d3.Added.Count -or $d3.Changed.Count) { $crash = $true; Say "CRASH after delete" 'CRASH'; break }

    Say "round $r complete, no crash" 'ok'
}

# ===================================================================== post
Head "POST"
$postStatus = Invoke-Ctl $ctl @('status') 'post status'
Invoke-Ctl $feat @('vols') 'post vols' | Out-Null
Invoke-Ctl $ctl @('log') 'kernel log (last)' | Out-Null

$uf = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}' -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters -join ','
$pa = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters' -ErrorAction SilentlyContinue
$fx = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features' -Name Flags -ErrorAction SilentlyContinue).Flags
Say "UpperFilters   = $uf"
Say "FilterArmed    = $($pa.FilterArmed)"
Say "WriteBack      = $($pa.WriteBack)"
Say "DeploySeq/Ack  = $($pa.DeploySeq)/$($pa.DeployAck)"
Say "Features\Flags = $fx"

# ---- P0 after-sample + MISS COUNTER READ-PATH-ONLY CONTROL ------------------
$p0After = Get-FeatSample 'P0 after'
Head "MISS COUNTER READ-PATH-ONLY CONTROL"
# Fail-closed default: if any early exit skips the control entirely, the state
# must read as "not evaluated", never as one of the three "as expected" states.
# There are exactly four states and 'UNREADABLE' is the only one that means
# "this control produced no usable reading".
$p0state = 'UNREADABLE'; $p0notes = New-Object System.Collections.Generic.List[string]
# Threshold, derived from a compile-time guarantee rather than chosen.
#
#   LIMIT = (legitimate READ bytes) / (max bytes one RbFeatOnMiss call can cover)
#
# The bound, traced through the actual call chain (verified in source, not
# assumed -- an earlier note in this file said "one call covers at most 131072 B"
# and then cited read_plan.h:29, but read_plan.h only asserts the SLICE size;
# the bound on what reaches RbFeatOnMiss comes from the loop below it):
#   1. RbReadWorker splits the request with rb_read_plan_chunk, which returns at
#      most RB_READ_PLAN_MAX_BYTES = 131072 (read_plan.c:15-22, read_plan.h:29).
#   2. rb_read() fills miss[] for that chunk, so every mr->len <= 131072.
#   3. RbReadMissRange passes either mr->len (>1 MB path) or a <=32 MB slice of
#      it (staging path, rbio_worker.c:288-296); both are therefore <= 131072.
#   4. RbFeatOnMiss adds exactly 1 interval per call (rbfeat.c:919).
# So dMisses intervals can only be explained by dMisses * 131072 bytes of read
# traffic. NOTE step 3 means the 32 MB staging loop is NOT a looser bound: its
# input is already capped by step 1, which is why the ceiling holds for both the
# >1 MB direct path (rbio_worker.c:256) and the staging path (:289).
#
# What may legitimately be READ in this run?
#   (a) ASSUMPTION (and it did NOT hold on this box): the copies. Each round does
#       Copy-Item A -> B, and Copy-Item is expected to READ its source, so
#       $Rounds * $SizeMB of read traffic would be "expected and fine".
#       MEASURED 2026-11-20 15:18:40: 读请求 331,322 at 15:18:53 and 331,322 at
#       15:19:09 -- delta 0 across both rounds. Copy-Item produced no volume read
#       requests at all, and dMisses stayed 0. Physical explanation (recorded, not
#       used as an excuse): A had just been written and Flush($true)d, so its
#       pages were still in the system cache / standby list and the copy's source
#       reads were satisfied from memory without an IRP_MJ_READ reaching the
#       filter. CONSEQUENCE: 14,400 is an UPPER BOUND derived from a premise that
#       this workload does not actually trigger -- it is NOT an expected value,
#       and a dMisses of 0 here carries no information about the write path. That
#       is why dMisses=0 gets its own state, ZERO-UNINFORMATIVE, below.
#   (b) NTFS metadata: a few MB, i.e. ~1,024..8,192 intervals at 4 KiB.
#   NOT the writes. Our own write bytes reached the volume through IRP_MJ_WRITE
#   (which P4 proves independently); if they show up as READ traffic, that IS
#   the anomaly this control exists to catch.
#
# So LIMIT = $Rounds * $SizeMB * 1MB / 131072 = 1,887,436,800 / 131,072 = 14,400
# for the default 2 x 900 MB -- but ONLY IF the copies really read from the
# volume (see (a) above: on this box they did not). So read it as an UPPER BOUND
# of the traffic this workload can produce, not as an expected value.
#
# Why the gate is 2x the anchor and NOT the anchor itself:
#   the anchor counts only the reads we can name in advance (the copies). A run
#   also performs reads we do not model per-instruction: NTFS metadata, directory
#   enumeration, the unaligned tail of each copy, Write-RealFile's own path
#   checks. Those are legitimate and could add a few thousand intervals. Sitting
#   the gate exactly on 14,400 would classify normal noise as an anomaly, and an
#   alarm that cries on a clean run gets switched off -- after which the real
#   anomaly is missed too. 2x leaves the noise room while still catching the
#   accident this control exists for by orders of magnitude (a write-path leak
#   would add ~460,000 intervals: 900 MB / 128 KiB x 2 rounds oversized by the
#   4 KiB-vs-128 KiB factor -- 16x above even the 2x gate).
#
# Three wrong drafts preceded this, all worth recording:
#   * 20,000 hand-picked: not derivable at all, and above 14,400, so it would
#     have missed the entire "all reads coalesced into 128 KiB" accident class.
#   * copy bytes PLUS write bytes = 28,800, justified as "the copies were
#     missing from the count": the ARITHMETIC was right but the REASON was
#     wrong -- it treated our own write bytes as legitimate read traffic, which
#     would grant a full write-path leak permission to look normal. The writes
#     were never legitimate read traffic (RbFeatOnMiss has two call sites, both
#     reads). The number survives as the gate; the justification is replaced by
#     the "unmodelled normal reads" argument above, which is falsifiable.
#   * gate == anchor (14,400): correct as a derivation, wrong as a threshold --
#     it leaves no room for legitimate unmodelled reads, so a clean run can be
#     reported as an anomaly.
$wCopyBytes = [int64]$Rounds * [int64]$SizeMB * 1MB      # each round copies one file
$p0Limit = [int64][math]::Floor($wCopyBytes / 131072.0)  # lower anchor: expected ceiling
$p0Gate  = $p0Limit * 2                                  # gate: anchor + 2x noise margin
if (-not ($p0Before.Ok -and $p0After.Ok)) {
    # UNREADABLE must be its own state, never folded into an "as expected" one:
    # a counter we could not read is not a counter that read a small number.
    # Lumping them together is the "empty set passes" family this repo has hit
    # twice -- it lets "instrument broken" masquerade as "everything normal".
    $p0state = 'UNREADABLE'
    $p0notes.Add("P0: one or both samples unreadable -> dMisses = MISSING (never 0). Read-path control not evaluated; this is NOT a pass.")
    Say "P0 dMisses    : MISSING (counter unreadable; NOT 0)" 'WARN'
    Say "P0 state      : UNREADABLE - the control did not run" 'WARN'
} else {
    $dMisses = $p0After.Misses - $p0Before.Misses
    $dBytesMiss = $p0After.BytesMiss - $p0Before.BytesMiss
    Say ("P0 dMisses    : {0:N0}   (before {1:N0} -> after {2:N0})" -f $dMisses, $p0Before.Misses, $p0After.Misses)
    Say ("P0 dBytesMiss : {0:N1} MB" -f ($dBytesMiss / 1MB))
    Say ("P0 anchor     : copyread/131072 = {0:N0}/{1} = {2:N0}  ({3} rounds x {4} MB copied -> UPPER BOUND, valid only if the copies actually read from the volume)" -f `
        $wCopyBytes, 131072, $p0Limit, $Rounds, $SizeMB)
    Say ("P0 gate       : anchor x2 = {0:N0}  (room for unmodelled normal reads: NTFS metadata, dir enumeration, unaligned copy tails)" -f $p0Gate)
    # ---- was the anchor's premise observed? ------------------------------------
    # The anchor says "the copies are the legitimate read traffic". That is a
    # testable claim: if the copies read 1.8 GB from the volume, the READ REQUEST
    # counter must move. It is printed unconditionally so that a dMisses of 0 can
    # be interpreted instead of merely recorded -- 0 misses after 0 read requests
    # is "nothing was asked to be read", not "reads happened and all hit".
    $snapPostP0 = Get-CounterSnapshot $postStatus
    $dReadReq = if ($null -ne $snapPre.ReadReq -and $null -ne $snapPostP0.ReadReq) {
        $snapPostP0.ReadReq - $snapPre.ReadReq } else { 'MISSING' }
    $copyReadObserved = ($dReadReq -ne 'MISSING') -and ($dReadReq -gt 0)
    Say ("P0 copyread   : dReadReq={0}  (pre {1} -> post {2})" -f `
            $(if ($dReadReq -eq 'MISSING') { 'MISSING' } else { '{0:N0}' -f $dReadReq }),
            $(if ($null -eq $snapPre.ReadReq) { 'MISSING' } else { '{0:N0}' -f $snapPre.ReadReq }),
            $(if ($null -eq $snapPostP0.ReadReq) { 'MISSING' } else { '{0:N0}' -f $snapPostP0.ReadReq })) `
        $(if ($copyReadObserved) { 'ok' } else { 'WARN' })
    if ($copyReadObserved) {
        Say "P0 anchor premise: OBSERVED - the copies issued volume reads, so the anchor applies" 'ok'
    } else {
        Say "P0 anchor premise: NOT OBSERVED - read requests did not move, so the copies were served from memory" 'WARN'
        Say "                   (A was just written and Flush(true)'d; its pages were still cached, so Copy-Item's" 'WARN'
        Say "                   source reads never became IRP_MJ_READ. The anchor is an upper bound, not an expectation.)" 'WARN'
        $p0notes.Add("P0: dReadReq=$dReadReq across the whole load -> the copies did NOT read from the volume (cache-served). The 14,400 anchor is an upper bound derived from a premise this workload does not trigger.")
    }
    # L2 gate FIRST: with L2 on, RbFeatOnMiss may skip the disk read entirely
    # (rbfeat.c:921-922 + rbio_worker.c:257/:290), so dMisses stops meaning
    # "disk I/O count" and even the control reading is distorted. A reading that
    # cannot mean what this control needs it to mean is UNREADABLE, and the note
    # records the actual reason so it is not confused with a counter fault.
    if (($p0Before.L2State -ne 0) -or ($p0After.L2State -ne 0)) {
        $p0state = 'UNREADABLE'
        $p0notes.Add("P0: L2state=$($p0Before.L2State)/$($p0After.L2State) (must be 0) -> dMisses no longer equals disk miss count; reading unusable, NOT a pass")
        Say "P0 L2 gate    : FAIL (L2state must be 0) -> state UNREADABLE" 'FAIL'
    } else {
        Say "P0 L2 gate    : L2state=0 both samples" 'ok'
        # ------------------------------------------------------------------
        # This is a CONTROL, not a D-01 judge.
        # D-01 §4.1 expects dMisses ~7.5e5..9.0e5, but that expectation comes
        # from `ramboostctl bench` reading 20,707.6 MB over 165,661 slices.
        # THIS script writes 900 MB x2, copies and deletes. The miss counter is
        # incremented by the READ path (RbFeatOnMiss has exactly two call sites,
        # rbio_worker.c:256 and :289, both reads). So a write-only workload must
        # produce dMisses near zero, and judging it against the bench band would
        # emit "D01-REFUTED" purely because we measured the wrong workload --
        # killing D-01 for a reason that is entirely our own measurement scope.
        # A judge that bites the wrong target is worse than no judge.
        #
        # What IS worth judging here, and costs nothing: whether the counter
        # stayed quiet. If dMisses is in the bench's 1.6e5..1.0e6 range after a
        # pure write load, then either the read path was somehow exercised by
        # writes, or the counter is being fed from somewhere unexpected. Both
        # are findings. The D-01 verdict itself belongs to the other script.
        # ------------------------------------------------------------------
        # FIVE disjoint states, ordered so exactly one is reachable.
        #   ZERO-UNINFORMATIVE is separated from ZERO-AS-EXPECTED because the
        #   first real run exposed the difference: dMisses=0 arrived together with
        #   dReadReq=0, i.e. nothing was ever asked to be read. Calling that
        #   "as expected" would dress an unexercised instrument up as a passing
        #   check -- the "empty set passes" family, one level up. A zero that
        #   cannot distinguish "the write path is clean" from "the read path never
        #   ran" carries no information, and must say so.
        #   ZERO-AS-EXPECTED now requires the anchor premise to have been observed
        #   (copy reads really happened) AND zero misses -- that would be a
        #   genuinely quiet counter.
        # The state itself comes from Get-P0State (pure function, sliceable), so
        # the replay harness exercises THESE bytes rather than a rewritten copy.
        # Only the operator-facing text is dispatched here on the returned state.
        $p0state = Get-P0State $dMisses $dReadReq $p0Before.L2State $p0After.L2State $p0Gate
        switch ($p0state) {
        'ZERO-AS-EXPECTED' {
            Say "P0 dMisses    : 0 with the copies really reading from the volume -> ZERO-AS-EXPECTED (read path ran, counter stayed quiet, write bytes did not leak)" 'ok'
            $p0notes.Add("P0: dMisses=0 while dReadReq=$dReadReq > 0. The read path was exercised and the miss counter stayed quiet; this is the informative-zero case.")
        }
        'ZERO-UNINFORMATIVE' {
            Say ("P0 dMisses    : 0 but dReadReq={0} -> ZERO-UNINFORMATIVE (the read path was never exercised, so this zero does NOT calibrate the counter)" -f $dReadReq) 'WARN'
            Say "                What it DOES still establish: the miss counter did not move at all while ~4 GB" 'ok'
            Say "                of writes crossed the volume. RbFeatOnMiss counts UNCONDITIONALLY (rbfeat.c:919," 'ok'
            Say "                Rb2Count(1,1) sits before the g_Rb2Flags gate), so a write path that called it" 'ok'
            Say "                would have moved dMisses even with zero reads. Hence: no write-path leak observed." 'ok'
            Say "                What it does NOT establish: that the miss counter would count reads correctly in" 'WARN'
            Say "                this session (the anchors 14,400 / 28,800 remain bounds, never measured here)." 'WARN'
            Say "                To make the anchor measurable the copies must be forced to read from the volume" 'WARN'
            Say "                (evict the source, or copy data this script did not just write in this session)." 'WARN'
            Say "                Until then read P0 as 'no write leak, read path unmeasured', and rely on P4 for" 'WARN'
            Say "                write-path coverage." 'WARN'
            $p0notes.Add("P0: dMisses=0 AND dReadReq=$dReadReq -> ZERO-UNINFORMATIVE for the counter-calibration claim, but still a positive result for the LEAK claim: a write-path leak would have moved the unconditional count at rbfeat.c:919 even with no reads. Reported, never gated.")
        }
        'LOW-AS-EXPECTED' {
            Say ("P0 dMisses    : 0 < {0:N0} <= {1:N0} -> LOW-AS-EXPECTED (explainable by copy reads + unmodelled metadata; write bytes did not leak into the read counter)" -f $dMisses, $p0Gate) 'ok'
            if ($dMisses -gt $p0Limit) {
                $p0notes.Add("P0: dMisses=$dMisses is above the 14,400 copy-read anchor but within the 28,800 gate -- i.e. between 0 and $([int64](($dMisses*131072)/1MB)) MB of read traffic is NOT from the copies. Normal for metadata, but record the number.")
            }
        }
        'HIGH-UNEXPECTED' {
            # The implied read traffic must be computed, not templated: an earlier
            # draft wrote this as a raw "{0:N0}" inside Say without a -f argument,
            # so the line printed the literal brace text. Caught by the stub test.
            $impliedMaxMB = [int64](($dMisses * 131072) / 1MB)
            Say ("P0 dMisses    : {0:N0} > {1:N0} -> HIGH-UNEXPECTED (WARN, does not gate rc)" -f $dMisses, $p0Gate) 'WARN'
            Say ("                Explaining {0:N0} intervals needs up to {1:N0} MB of read traffic" -f $dMisses, $impliedMaxMB) 'WARN'
            Say "                (131072 B each is the per-call MAXIMUM, so the real figure could be" 'WARN'
            Say "                smaller - 4 KiB intervals would imply only 1/32 as much). Either way" 'WARN'
            Say "                it is more than this load can legitimately produce: the read path was" 'WARN'
            Say "                exercised, or the counter is fed unexpectedly. Report it." 'WARN'
            $p0notes.Add("P0: dMisses=$dMisses exceeds the 2x gate $p0Gate (anchor $p0Limit = copyread/131072; legitimate READ bytes = copy reads only, our own writes are NOT read traffic) -> at least $([int64]($dMisses*131072/1MB)) MB of read traffic was counted, more than this load can produce. Not a D-01 verdict, but must be explained.")
        }
        default {
            $p0notes.Add("P0: state=$p0state (L2 gate or unreadable counter) -> reading unusable, NOT a pass")
            Say "P0 state      : $p0state - the control did not run as specified" 'WARN'
        }
        }
        # N2 retained: |dBytesMiss - dMisses*4096|/dBytesMiss is an internal
        # consistency relation, independent of which workload produced it.
        if ($dBytesMiss -gt 0) {
            $dev = [math]::Abs($dBytesMiss - ($dMisses * 4096)) / $dBytesMiss
            Say ("P0 N2         : |dBytesMiss - dMisses*4096|/dBytesMiss = {0:P2} (accept < 2%; load-shape independent)" -f $dev) `
                $(if ($dev -lt 0.02) { 'ok' } else { 'FAIL' })
            if ($dev -ge 0.02) {
                $p0notes.Add("P0: N2 deviation $('{0:P2}' -f $dev) >= 2% -> miss interval is not 4 KiB; explain before optimising")
            }
        } else {
            Say "P0 N2         : dBytesMiss=0 -> cannot test (MISSING, not PASS)" 'WARN'
            $p0notes.Add("P0: N2 untestable (dBytesMiss=0)")
        }
    }
}
Say "P0 state      : $p0state"
foreach ($n in $p0notes) { Say "note: $n" 'WARN' }

$dumpFinal = Compare-Dumps $snap0
Head "DUMP COMPARISON (single check against the baseline)"
Say "added   : $($dumpFinal.Added.Count)  $(if ($dumpFinal.Added.Count) { $dumpFinal.Added -join ', ' })"
Say "changed : $($dumpFinal.Changed.Count)  $(if ($dumpFinal.Changed.Count) { $dumpFinal.Changed -join ', ' })"
Say "removed : $($dumpFinal.Removed.Count)  $(if ($dumpFinal.Removed.Count) { $dumpFinal.Removed -join ', ' })"
# Added / Changed  = crash evidence (someone or something wrote a new dump, or
#                    overwrote an existing one).
# Removed          = NOT crash evidence.  A user or a cleanup tool deleting an
#                    old .dmp says nothing about whether the kernel bugchecked;
#                    it only invalidates the baseline.  Treating removal as a
#                    crash would fail a clean run for an unrelated reason.
#                    So: WARN + explicit "baseline integrity degraded" note.
if ($dumpFinal.Added.Count -or $dumpFinal.Changed.Count) { $crash = $true }
if ($dumpFinal.Removed.Count) {
    $hiDumpRemoved = $dumpFinal.Removed.Count
    Say "removed baseline dump(s): crash detection integrity degraded (NOT counted as a crash)" 'WARN'
    $verdicts.Add("P1 baseline: $($dumpFinal.Removed.Count) baseline dmp(s) disappeared - detection incomplete")
}

# ================================================================== verdict
Head "VERDICT"
$p1 = -not $crash
$p2 = -not ($verdicts | Where-Object { $_ -match 'P2' })
$p3 = -not ($verdicts | Where-Object { $_ -match 'P3' })
$p5 = ($uf -match 'RAMBlockCache') -and ($pa.FilterArmed -eq 0) -and ($pa.WriteBack -eq 0) -and ($fx -eq 0)

# ---- P4: prove THIS load crossed the filter (byte delta, not "it moved") ----
# Unit check (verified, because a GB/MB slip here would silently corrupt the
# ratio): the tool prints '%.2f GB' of bytes/1024^3 (ramboostctl.c:97), and
# PowerShell's 1GB is also 1024^3, so the two agree exactly.
#
# PRECISION BUDGET -- derivation, because the next reader sees the ratio bands
# and has no way to know whether the instrument can even resolve a 10% band:
#   * the tool prints bytes/1024^3 with '%.2f' (ramboostctl.c:97)
#     => one printed step       = 0.01 GB = 10.24 MB
#     => each reading is rounded to 0.005 GB = 5.12 MB  (half a step)
#   * dWrite = after - before, so the two roundings can oppose each other
#     => worst-case delta error  = 2 * 5.12 MB = 10.24 MB
#   * the per-round denominator is SizeMB*2 = 1,800 MB (A created + B copied),
#     so relative error = 10.24 / 1800 = 0.57%
#     and for the run total (3,600 MB) = 10.24 / 3600 = 0.28%
#   * the narrowest band that matters is [0.90, 1.15], i.e. it must resolve
#     0.90 vs 1.00 vs 1.15 => the tightest gap is 10 percentage points, and
#     0.57% of noise eats 5.7% of that gap (0.28% eats 2.8% on the run total).
#   CONCLUSION: at 900 MB/round the instrument resolves the bands with an order
#   of magnitude to spare, and a quantisation error alone can never move a case
#   from PASS to FAIL: the nearest band edge (1.15) is 15 points away while the
#   noise is 0.57 points.
#   This is NOT a constant of the script: SizeMB is a parameter, so the budget is
#   COMPUTED below from the ACTUAL expected bytes and PRINTED rather than
#   asserted in a comment. A judge whose precision is only claimed in prose is a
#   judge nobody can re-derive.
$quantStepBytes  = [double]0.01 * 1GB          # one printed step of '%.2f GB'
$deltaErrBytes   = $quantStepBytes             # two half-steps, worst case
$snapPost = Get-CounterSnapshot $postStatus
$dWrite = Format-CounterDelta $snapPre $snapPost 'WriteBytes'
# dRead is REPORTED ONLY, never a gate. And it is expected to be MISSING:
# `ramboostctl status` prints no read-byte field at all (ramboostctl.c:96-100 --
# read side is a request COUNT plus two hit PERCENTAGES). The reason is carried
# into the report so that MISSING is not read as "instrument broken".
$dRead  = Format-CounterDelta $snapPre $snapPost 'ReadBytes'
$dReadWhy = if ($dRead -eq 'MISSING') { $snapPost.ReadSource } else { '' }
$writeReqDelta = if ($null -ne $snapPre.WriteReq -and $null -ne $snapPost.WriteReq) {
    $snapPost.WriteReq - $snapPre.WriteReq } else { 'MISSING' }
# ---- P4 denominator: EVERY IRP_MJ_WRITE this run causes --------------------
# Copy-Item is NOT a server-side copy here. The first real run (2026-11-20
# 15:18:40) settled that by measurement, not by assumption:
#   write-request delta = 13,526 for a run whose payload is 900+900 MB written
#   directly plus 900+900 MB copied  => ~4 requests per MB, i.e. the copy really
#   produced its own IRP_MJ_WRITEs; and the observed byte delta was 3,655.68 MB
#   against 3,600 MB of payload (+1.55%, which is NTFS metadata).
# So the denominator must be direct writes PLUS copy writes. An earlier version
# used $totalWriteMB alone (= direct writes only = 1,800 MB), which made the
# ratio 2.03 on a perfectly healthy run -- a scope error in the DENOMETER that
# the old one-sided judge (ratio >= 0.9) then waved through as PASS. Both halves
# are now printed so the next reader can see the difference between the口径s
# instead of having to derive it.
$expectedDirectOnlyBytes = [double]$totalWriteMB * 1MB
# Why the else branch is DEAD CODE under the current rc contract -- and stays
# documented rather than deleted or turned into a throw:
#   $roundExpectedBytes gets its first element immediately after S1's write
#   returns, i.e. before the first `break` in the round (:864). So `.Count` is 0
#   only if the loop body never reached :864 at all. Two ways that can happen,
#   and both are already fail-closed:
#     (a) the round body throws before :864 -> $ErrorActionPreference='Stop' plus
#         the trap at :701 prints UNCAUGHT and does `exit 4`, so this line is
#         never evaluated; the run has already failed.
#     (b) $Rounds is 0 -> physically impossible: [ValidateSet(1,2)] on :43, and
#         the .cmd passes `-Rounds 2`. A 0 would be rejected at the call.
#   So the branch does not fire today. It is kept as a belt-and-braces because
#   it is the widest-blast-radius fallback in the file: if a future edit lets the
#   loop body skip :864 without throwing (e.g. `continue` instead of `break`),
#   the denominator would fall back to $totalWriteMB + $totalCopyMB, which is
#   the same number for a completed round -- so the P4 ratio would stay right
#   rather than silently halving. Converting it to a `throw` would be stricter
#   but would turn a currently-unreachable path into a hard abort with no test
#   covering it; the documented fallback is safer under the 2-round budget.
#   Also note the measured behaviour of the fallback expression itself: an empty
#   list gives `(Measure-Object -Sum).Sum == 0` as a Double, NOT $null, so
#   `[double]$null` -> 0 would not be the failure mode here -- 0 is. A 0
#   denominator is caught downstream by Get-P4RatioState (`<= 0` -> FAIL), which
#   is where the gate belongs.
$expectedWriteBytes = if ($roundExpectedBytes.Count) {
    [double](($roundExpectedBytes | Measure-Object -Sum).Sum)
} else {
    [double]($totalWriteMB + $totalCopyMB) * 1MB
}
$p4 = 'UNKNOWN'; $p4note = ''
if ($dWrite -eq 'MISSING') {
    $p4 = 'MISSING'
    $p4note = 'write-byte counter unreadable (null / went backwards -> not elevated?)'
} else {
    $p4 = Get-P4RatioState ([double]$dWrite) $expectedWriteBytes
    $p4note = Format-P4RatioNote ([double]$dWrite) $expectedWriteBytes $p4
    if ($p4 -eq 'FAIL') {
        $verdicts.Add("P4 FAIL: dWrite=$('{0:N2}' -f ([double]$dWrite/1MB)) MB vs expected $('{0:N2}' -f ($expectedWriteBytes/1MB)) MB (ratio $('{0:P2}' -f ([double]$dWrite/$expectedWriteBytes))) is at or outside (0.5, 2.0)")
    } elseif ($p4 -eq 'WARN-LOW') {
        $verdicts.Add("P4 note: partial attribution $('{0:P2}' -f ([double]$dWrite/$expectedWriteBytes)) (>50%, <90%)")
    } elseif ($p4 -eq 'WARN-HIGH') {
        $verdicts.Add("P4 note: over-attribution $('{0:P2}' -f ([double]$dWrite/$expectedWriteBytes)) (>115%, <200%) - more bytes crossed the filter than our own writes explain; look for another writer before accepting")
    }
}
Say "P1 no crash (new/changed minidump)          : $(if ($p1) { 'PASS' } else { 'FAIL' })"
Say "P2 VDL asserted every write and copy        : $(if ($p2) { 'PASS' } else { 'FAIL' })"
Say "P3 clusters really freed (>=50% reclaimed)  : $(if ($p3) { 'PASS' } else { 'FAIL' })"
Say "P4 this load crossed the filter (byte delta): $p4   $p4note"
Say ("   dWrite={0} dRead={1} expectedWrite={2:N2} MB" -f `
        $(if ($dWrite -eq 'MISSING') { 'MISSING' } else { '{0:N2} MB' -f ([double]$dWrite/1MB) }), `
        $(if ($dRead  -eq 'MISSING') { 'MISSING' } else { '{0:N2} MB' -f ([double]$dRead /1MB) }), `
        [double]($expectedWriteBytes / 1MB))
# Both denominators, side by side: the口径 difference must be visible, never
# silently resolved in favour of whichever one makes the run look good.
Say ("   expected breakdown: direct-write only={0:N2} MB (A created) + copy-write={1:N2} MB (B produced by Copy-Item) = {2:N2} MB total" -f `
        ($expectedDirectOnlyBytes / 1MB), (($expectedWriteBytes - $expectedDirectOnlyBytes) / 1MB), ($expectedWriteBytes / 1MB))
Say ("     (an earlier version used only the {0:N2} MB figure as the denominator -> ratio came out 2.03 on a healthy run)" -f `
        ($expectedDirectOnlyBytes / 1MB)) 'warn'
if ($dReadWhy) { Say ("   dRead MISSING is expected: {0}" -f $dReadWhy) 'warn' }
# The precision budget that the 90% gate depends on, printed so the reader does
# not have to trust the comment. If SizeMB is ever lowered, this line starts
# reporting a large percentage and the gate's resolvability becomes visible.
Say ("   P4 precision : one printed step=0.01 GB={0:N2} MB; worst dWrite error=+-{1:N2} MB; per round={2:N0} MB (A+B) -> noise={3:P2} of a round, {4:P2} of the run total {5:N0} MB" -f `
        ($quantStepBytes / 1MB), ($deltaErrBytes / 1MB), ($SizeMB * 2), `
        $(if (($SizeMB*2) -gt 0) { $deltaErrBytes / ([double]$SizeMB * 2 * 1MB) } else { 1.0 }), `
        $(if ($expectedWriteBytes -gt 0) { $deltaErrBytes / $expectedWriteBytes } else { 1.0 }), `
        ($expectedWriteBytes / 1MB))
if ($snapPre.WriteBytesLine -or $snapPost.WriteBytesLine) {
    Say ("   raw counter  : pre=[{0}] post=[{1}]" -f $snapPre.WriteBytesLine, $snapPost.WriteBytesLine)
} else {
    Say "   raw counter  : both lines unreadable (see P4 above)" 'warn'
}
Say ("   writeReq delta={0}  (cross-check: the request counter must move with the byte counter)" -f `
        $(if ($writeReqDelta -eq 'MISSING') { 'MISSING' } else { '{0:N0}' -f $writeReqDelta }))
# Per-round attribution is the real guard against "round 1 never crossed, round 2
# did, and the single pre->post delta looked fine". Print it here every run so the
# report carries the per-round numbers, not just the run total.
if ($roundP4Notes.Count) {
    Say "   per-round P4 attribution:"
    foreach ($rp in $roundP4Notes) { Say ("     {0}" -f $rp) }
}
Say "P5 config unchanged / still filtered        : $(if ($p5) { 'PASS' } else { 'FAIL' })"
Say "meta: whole-volume flush (NTFS metadata)   : $(if ($metaFlushOk) { 'fsutil volume flush OK' } else { 'FAILED - metadata write path NOT proven' })"
Say ""
Say ("totals: write {0} MB / copy {1} MB / delete {2} MB" -f $totalWriteMB, $totalCopyMB, $totalDelMB)
if ($writeRates.Count) {
    $avg = ($writeRates | Measure-Object -Average).Average
    Say ("write rate per round: {0} MB/s (avg {1:N1}) - LOW, recorded as observed, not normalised away" -f
         (($writeRates | ForEach-Object { '{0:N1}' -f $_ }) -join ', '), $avg)
}
foreach ($v in $verdicts) { Say "note: $v" 'WARN' }

if (-not $crash) {
    Say ""
    Say "BOUNDARY (read this before quoting any result):" 'warn'
    Say "  This is 'NOT REPRODUCED', not 'FIXED'." 'warn'
    Say "  One machine, one load shape, $Rounds rounds of $SizeMB MB." 'warn'
    Say "  Verifier covered RAMBlockCache.sys only (0x1209BB), not globally." 'warn'
}

Say ("total wall = {0:N1} min" -f ((Get-Date) - $t0All).TotalMinutes)

# Terminal marker.  Written to the durable log so that later, from the outside,
# "did the run finish or was it killed?" is answerable without guessing: an
# RUNABORTED marker with no TERMINAL line means the process died mid-run (which
# for this task is itself evidence).
# P0 is deliberately absent from this expression: it is a CONTROL, reported in
# the TERMINAL line as P0=<state>, never a gate on rc. Making it gate would let a
# counter that cannot be read turn a crash-free run into a FAIL, and would let a
# NONZERO/HIGH-UNEXPECTED (which may be ordinary metadata) fail an otherwise
# clean run. NOTE: the control's own FIVE states are UNREADABLE /
# ZERO-AS-EXPECTED / ZERO-UNINFORMATIVE / LOW-AS-EXPECTED / HIGH-UNEXPECTED.
# 'UNREADABLE' is the fail-closed default (no usable reading); 'ZERO-UNINFORMATIVE'
# is the honest name for "the counter read zero because nothing was asked to be
# read" -- measured on this box, where the copies were served from cache. Neither
# of those is a pass, and neither is a failure.
$hardFail  = $crash -or (-not ($p2 -and $p3 -and $p5)) -or ($p4 -eq 'FAIL')
$inconclusive = (-not $hardFail) -and ($p4 -ne 'PASS')
if ($hardFail) { $rc = 3 } elseif ($inconclusive) { $rc = 5 } else { $rc = 0 }
# dMisses for the TERMINAL line, computed once: MISSING unless BOTH samples were
# readable, so an unreadable counter can never surface as a number (a subtraction
# of two nulls would otherwise print either 0 or a negative).
$p0TermMisses = if ($p0Before.Ok -and $p0After.Ok) { $p0After.Misses - $p0Before.Misses } else { 'MISSING' }
Write-LogLine ("TERMINAL rc={0} crash={1} P1={2} P2={3} P3={4} P4={5} P5={6} P0={7} dMisses={8} metaFlush={9} wall={10:N1}min" -f `
    $rc, $crash, $p1, $p2, $p3, $p4, $p5, $p0state, $p0TermMisses, `
    $metaFlushOk, ((Get-Date) - $t0All).TotalMinutes)
if ($LogPath) { Say "log appended: $LogPath" }
exit $rc