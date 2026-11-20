# RB-bench-miss-coalescing.ps1 - D-01 P0: quantify dMisses with an algebraic split
#
# WHAT THIS ANSWERS (one question, nothing else):
#   Does the second bench pass really issue 4 KiB disk reads (fragmented misses,
#   D-01's premise), or does it also coalesce into 128 KiB (misses are clustered,
#   D-01 refuted, candidate D not worth building)?
#
# WHY WE CANNOT JUST ISOLATE THE SECOND PASS:
#   `ramboostctl bench` calls CliFlushFileRange() once, before pass 1
#   (ctl_bench.c:386). So two separate `-passes 1` invocations are
#   flush->cold-read->flush->cold-read: BOTH are cold, and pass 2 would report
#   165,660 x 128 KiB intervals -> a confident FALSE "D-01 refuted".
#   Verified in source before writing this script.
#
# THE ALGEBRAIC DECOMPOSITION (why one `-passes 2` is enough):
#   B  = file bytes                         (measured from the file itself)
#   S  = slices = ceil(B / 131072)          (128 KiB slice size)
#   h  = secondHitRate                      (printed by bench; its window is
#                                            exactly pass 2: s0 after pass 1 at
#                                            ctl_bench.c:408, s1 after pass 2
#                                            at :420)
#   Pass 1 misses everything and coalesces one interval per slice: m1 = B
#   Pass 2 misses m2 = B * (1 - h)
#   External stats delta covers BOTH passes:
#       dBytesMiss_expected = B + m2 = B * (2 - h)
#       dMisses_expected    = S + m2 / r2
#   Solve for the unknown r2, the mean interval size of pass 2:
#       r2 = m2 / (dMisses - S)
#   r2 is a directly solved physical quantity, not a band guess: 4096 vs 131072
#   differ by 32x, so there is no room for misclassification.
#
#   Key property: coalescing changes INTERVAL COUNT, never BYTE COUNT. So the
#   byte side (N2) is an independent self-check of the same run.
#
# KNOWN SANITY VALUES on this file (B=21,713,462,848, h=0.8445), recomputed here:
#   S=165,661  m1=B  m2=B*(1-h)=3,376,443,473 B  dMisses~989,988  r2=4,096.0 B
#   (the lead's 3,376,414,720 / 989,987 differ by <0.001%; either is inside the
#    +-3% band, so no criterion depends on the third digit)
#
# GATE ORDER (hard: a later tier may not be evaluated if an earlier one fails):
#   G0a passes==2    G0 bench rc==0            G1 driverCold=='true'
#   G2 bench bytes==B                          G3 L2state==0 at both samples
#   G4 miss/bm readable at both samples        G5 secondHitRate printed and >=0
#   N2 byte side -> if it fails the model is wrong => INVALID, r2 NOT reported
#   r2 bands     -> CONFIRMED / MORE-FRAGMENTED / REFUTED / INCONCLUSIVE / INVALID
#
# RED LINES (hard-coded, not configurable):
#   * reads EXACTLY ONE file: D:\大模型\28391d06...  with GENERIC_READ semantics
#     (bench itself opens GENERIC_READ + FILE_FLAG_NO_BUFFERING, ctl_bench.c:328)
#   * any other path under D:\大模型 -> Fail-Early, no exception
#   * no write, no attribute change, no delete, no new file anywhere
#   * never creates/touches D:\rb-write-test (that belongs to the write-load script)
#   * read-only on Features\Flags / WriteBack / Verifier / UpperFilters
#   * no deploy, no reboot, no git commit, zero .c/.h, zero rebuild
#
# EXIT CODES (each code means exactly one thing -- no code carries two meanings):
#   0 = D01-CONFIRMED (or CONFIRMED-MORE-FRAGMENTED): premise holds
#   3 = D01-REFUTED or INVALID (a hard gate failed / the model broke)
#   4 = red line or abort    5 = MISSING (a required counter or field unreadable)
#   6 = D01-INCONCLUSIVE      (usable data, in neither band -- NOT 3 and NOT 5)

[CmdletBinding()]
param(
    [string]$BenchFile = 'D:\大模型\28391d0650082e170d19c33379c2fb7b11018d3acc50fd99b60930965ce3a395',
    # Exactly 2, not "2 or more": the algebra below divides the external miss
    # delta into "pass 1" and "pass 2" halves, and bench's hit-rate window is
    # passes 2..N. Only N=2 makes that window a single pass. Enforced as a red
    # line before elevation (see below), not merely as a comment.
    [int]$Passes = 2,
    [switch]$DryRun,
    [string]$LogPath = '',
    [switch]$NoElevate   # internal: set when the elevated child re-invokes us
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# bench / ramboostctl print Chinese on some lines ('[错误]', '已取消'). The console
# here is GB2312 and the tools emit UTF-8, so without this pin the text arrives as
# mojibake (measured: '[错误]' -> '[閿欒]'). Same fix as the write-load script.
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

if (-not $LogPath) {
    $LogPath = Join-Path $env:TEMP ('RB-bench-miss_{0}.log' -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
}

# ------------------------------------------------------- earliest failure path
# Red lines below run BEFORE Say/trap exist, and a trap only applies after its
# statement has executed. Fail-Early uses only built-ins so a rejection can
# still be logged and still return rc=4. (This ordering bug was found and fixed
# in the sibling script; do not reintroduce it here.)
function Fail-Early([string]$message) {
    Write-Host "  [ABORT] $message" -ForegroundColor Red
    if ($LogPath) {
        try {
            Add-Content -LiteralPath $LogPath -Encoding UTF8 -ErrorAction Stop -Value @(
                ("{0}   [ABORT] {1}" -f (Get-Date -Format 'HH:mm:ss'), $message),
                ("RUNABORTED {0} :: {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $message)
            )
        } catch { }
    }
    exit 4
}

# --------------------------------------------------------------- red lines
try {
    # Exactly one permitted file. Anything else under the sacred directory is
    # refused -- including a different file the user might have meant.
    $ALLOWED = 'D:\大模型\28391d0650082e170d19c33379c2fb7b11018d3acc50fd99b60930965ce3a395'
    $canon = [IO.Path]::GetFullPath($BenchFile).TrimEnd('\')
    if ($canon -ne $ALLOWED) {
        throw "RED LINE: this script may read exactly '$ALLOWED'; refusing '$canon'. Nothing else under D:\大模型 is permitted."
    }
    if (-not (Test-Path -LiteralPath $canon -PathType Leaf)) {
        throw "RED LINE: target file missing: $canon"
    }
    if ($Passes -lt 2) {
        throw "RED LINE: -Passes must be exactly 2, got $Passes. With -passes 1 there is no hot pass, and two such runs are flush+cold twice (ctl_bench.c:386), which would fake a 'miss clustered' verdict."
    }
    if ($Passes -ne 2) {
        # Checked HERE, before elevation, not only at gate G0a. The algebra
        # m2 = B*(1-h) is derived for exactly one cold and one hot pass; with
        # N>2 the hit-rate window (ctl_bench.c:408..420) covers passes 2..N and
        # cannot be split per pass. Deferring this to G0a would spend the user's
        # UAC click and a full 43 GB read before declaring a verdict that is
        # knowable for free -- a gate placed after the expensive work is a gate
        # in the wrong place.
        throw "RED LINE: -Passes must be exactly 2, got $Passes. With N>2 the secondHitRate window spans passes 2..N, so r2 would be a mixture of interval sizes rather than a per-pass measurement."
    }
    # Defensive: the write-load target must not appear anywhere in this script's
    # behaviour. If someone later wires it in, this trips.
    if ($BenchFile -like 'D:\rb-write-test*') {
        throw "RED LINE: D:\rb-write-test belongs to the write-load script, not this one"
    }
} catch {
    Fail-Early $_.Exception.Message
}

# ------------------------------------------------------------ logging / UI
$script:log = New-Object System.Collections.Generic.List[string]
if ($LogPath) {
    $hdr = "# RB-bench-miss durable log  start={0}  pid={1}  elevated-session={2}" -f `
        (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $PID, [bool]$NoElevate
    try {
        if ($NoElevate) { Add-Content -LiteralPath $LogPath -Value $hdr -Encoding UTF8 -ErrorAction Stop }
        else { Set-Content -LiteralPath $LogPath -Value $hdr -Encoding UTF8 -ErrorAction Stop }
    } catch { }
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

# -------------------------------------------------------------- tool paths
$repoRoot = 'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包'
$bench = Join-Path $repoRoot 'src\ctl\ramboostctl.exe'
$feat  = Join-Path $repoRoot 'src\ctl\rbfeatctl.exe'

# --------------------------------------------------- stats sampling (two ways)
# JSON carries miss (%llu) and bm (%llu raw BytesMiss) -- exact integers, no
# quantization. The plain form carries L2state, which the JSON branch does NOT
# print (rbfeatctl.c:592-601 vs :615). So we take BOTH and use each for what it
# is the only source of.
function Get-FeatJson([string]$label) {
    $o = [pscustomobject]@{ Label = $label; Hits = $null; Misses = $null
                            BytesMiss = $null; Ok = $false; Raw = $null }
    if (-not (Test-Path -LiteralPath $feat)) {
        Say "$label : rbfeatctl missing -> MISSING" 'WARN'; return $o
    }
    $out = & $feat @('stats', '-json') 2>&1
    $rc = $LASTEXITCODE
    $text = ($out | Out-String).Trim()
    $o.Raw = $text
    if ($rc -ne 0) {
        Say "$label : rc=$rc -> MISSING (NOT 0)" 'WARN'
        return $o
    }
    if ($text -match '"miss":(\d+)') { $o.Misses = [int64]$Matches[1] }
    if ($text -match '"bm":(\d+)')   { $o.BytesMiss = [int64]$Matches[1] }
    if ($text -match '"hits":(\d+)') { $o.Hits = [int64]$Matches[1] }
    $o.Ok = ($null -ne $o.Misses) -and ($null -ne $o.BytesMiss)
    if (-not $o.Ok) {
        Say "$label : JSON missing miss/bm -> MISSING (NOT 0). raw=$text" 'WARN'
    } else {
        Say "$label : miss=$($o.Misses) bm=$($o.BytesMiss)" 'ok'
    }
    Write-LogLine ("RAW[{0}] {1}" -f $label, $text)
    return $o
}
function Get-L2State([string]$label) {
    # L2state exists ONLY in the plain (non-JSON) branch.
    if (-not (Test-Path -LiteralPath $feat)) { return $null }
    $out = & $feat @('stats') 2>&1
    if ($LASTEXITCODE -ne 0) {
        Say "$label : L2state unreadable (rc=$LASTEXITCODE) -> MISSING" 'WARN'
        return $null
    }
    $text = ($out | Out-String)
    Write-LogLine ("RAW[{0}] {1}" -f $label, ($text.Trim() -replace "`r?`n", ' | '))
    if ($text -match 'L2state=(\d+)') { return [int64]$Matches[1] }
    return $null
}

# =================================================================== plan/dry
$isAdmin = Test-Elevated
$t0All = Get-Date
$B = [int64](Get-Item -LiteralPath $canon -Force).Length
$SLICE = [int64]131072                                              # 128 KiB
$S = [int64][math]::Ceiling($B / [double]$SLICE)

Head "PLAN (dry run=$([bool]$DryRun), elevated=$isAdmin)"
Say "file        : $canon"
Say ("file bytes  : B = {0:N0}  ({1:N2} GB)" -f $B, ($B / 1GB))
Say ("slices      : S = ceil(B/131072) = {0:N0}" -f $S)
Say "passes      : $Passes (must be exactly 2; pass 1 cold, pass 2 hot. -Passes 3+ is"
Say "              refused before elevation because the hit-rate window spans 2..N)"
Say "wall clock  : $($t0All.ToString('yyyy-MM-dd HH:mm:ss'))"
Say "durable log : $LogPath"
Say ""
Say "commands that WILL run (in this exact order):" 'plan'
Say "  0) elevate once: Start-Process -Verb RunAs with -DryRun stripped" 'plan'
Say "  1) rbfeatctl stats -json   -> miss, bm      (before)" 'plan'
Say "  2) rbfeatctl stats         -> L2state       (before; JSON has no L2state)" 'plan'
Say "  3) ramboostctl bench `"$canon`" -passes $Passes    (GENERIC_READ, no-buffering)" 'plan'
Say "  4) rbfeatctl stats -json   -> miss, bm      (after)" 'plan'
Say "  5) rbfeatctl stats         -> L2state       (after)" 'plan'
Say "  6) gate G0..G5, then N2 (model gate), then r2 bands (D-01 verdict)" 'plan'
Say ""
Say "judgement (ordered gates; a later tier only runs if every earlier gate passes):" 'ok'
Say "  G0a passes             must be exactly 2 (already enforced as a red line)" 'ok'
Say "  G0 bench rc            must be 0" 'ok'
Say "  G1 driverCold          must be 'true'. 'false' => CliFlushFileRange failed => pass 1" 'ok'
Say "                         was NOT cold => m1=B is false => INVALID, model not computed." 'ok'
Say "  G2 bench bytes         must equal B read from the file. A short/long read would" 'ok'
Say "                         make m1/m2 algebra wrong." 'ok'
Say "  G3 L2state==0          at BOTH samples. With L2 serving hits the disk read is" 'ok'
Say "                         skipped, so dMisses would no longer count disk I/O." 'ok'
Say "  G4 miss/bm integers    both samples, both fields, or MISSING (never 0)." 'ok'
Say "  G5 secondHitRate       printed by bench and >= 0. -1 means driver offline =>" 'ok'
Say "                         INVALID (must NOT be silently treated as h=0)." 'ok'
Say "  ---- tier 1: model gate (independent of any coalescing assumption) ----" 'ok'
Say "  N2  abs(dBytesMiss - B*(2-h)) / dBytesMiss < 2%  -> BYTES-CONSISTENT" 'ok'
Say "      else BYTES-INCONSISTENT => INVALID; r2 is NOT reported as a D-01 answer." 'ok'
Say "  ---- tier 2: D-01 judgement (only if N2 passed) ----" 'ok'
Say "  r2 = m2/(dMisses - S)   (mean interval bytes of pass 2)" 'ok'
Say "    CONFIRMED                r2 in [3768, 4424]      (4096 +-8%)" 'ok'
Say "    CONFIRMED-MORE-FRAGMENTED r2 < 3768               (more fragmented than" 'ok'
Say "                             D-01 assumed: premise still holds, with margin)" 'ok'
Say "    REFUTED                  r2 in [117965, 144179]  (131072 +-10%)" 'ok'
Say "    INCONCLUSIVE             r2 in (4424, 117965)    mixed state, no verdict" 'ok'
Say "    INVALID                  r2 > 144179             (beyond even the noise" 'ok'
Say "                             allowance for the 128 KiB slice ceiling => broken)" 'ok'
Say "  printed together: the six intermediates, the four raw counters, both means," 'ok'
Say "  and the r2 resolution (denominator size) so REFUTED is not read as equal" 'ok'
Say "  evidence strength to CONFIRMED (denominators differ ~32x)." 'ok'
Say ""
Say "what this will NOT prove:" 'warn'
Say "  * NOT that candidate D speeds anything up (that needs the QD curve; D-01 §7 P1)" 'warn'
Say "  * NOT anything about the write path (separate script, separate red lines)" 'warn'
Say "  * r2 is a MEAN; it cannot show the interval-size distribution" 'warn'

if ($DryRun) {
    Say ""
    Say "DRY RUN: nothing read, nothing written. Re-run without -DryRun." 'DRYRUN'
    Write-LogLine ("DRYRUN-END {0}" -f (Get-Date -Format 'HH:mm:ss'))
    Write-Host ""
    Write-Host "durable log: $LogPath"
    exit 0
}

# =================================================================== elevate
if (-not $isAdmin -and -not $NoElevate) {
    Head "ELEVATING (single UAC)"
    $self = $PSCommandPath
    # Native argument list via $PSCommandPath: no -Command string, so the
    # non-ASCII repo path cannot be re-parsed under the console codepage.
    $argList = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$self`"",
        '-BenchFile', "`"$canon`"", '-Passes', $Passes, '-NoElevate', '-LogPath', "`"$LogPath`""
    )
    $childExe = 'C:\Program Files\PowerShell\7\pwsh.exe'
    $childIsPwsh7 = Test-Path -LiteralPath $childExe
    if (-not $childIsPwsh7) {
        $childExe = 'powershell.exe'
        Say "pwsh 7 NOT found - falling back to Windows PowerShell 5.1" 'WARN'
    }
    Say "re-launching elevated: $childExe $($argList -join ' ')"
    Write-LogLine ("interpreter: {0} (pwsh7={1})" -f $childExe, $childIsPwsh7)
    Write-LogLine ("PARENT: launching elevated child at {0}" -f (Get-Date -Format 'HH:mm:ss'))
    Start-Process -FilePath $childExe -Verb RunAs -ArgumentList $argList

    # Propagate the child's REAL rc by tailing the durable log. A bare exit 0
    # would report "UAC cancelled" and "judged" identically -- the fake-success
    # class this repo keeps re-hitting.
    Say "waiting for the elevated child to report TERMINAL (up to 20 min) ..."
    $deadline = (Get-Date).AddMinutes(20)
    $sawChild = $false; $terminal = $null
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
        if ($terminal -match 'rc=(\d+)') { exit [int]$Matches[1] }
        if ($terminal -match '^RUNABORTED ') { exit 4 }
        exit 3
    }
    Say "child produced no TERMINAL/RUNABORTED line within 20 min - inspect the log" 'FAIL'
    exit 3
}

# ================================================================== execute
if (-not $isAdmin) {
    Say "not elevated and -NoElevate set -- counters will be unreadable." 'WARN'
}

trap {
    $msg = $_.Exception.Message
    Say "UNCAUGHT: $msg" 'ABORT'
    Write-LogLine ("RUNABORTED {0} :: {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg)
    Write-LogLine "  at: $($_.InvocationInfo.PositionMessage -replace "`r?`n", ' | ')"
    exit 4
}

Head "PRE-SAMPLES"
$l2Before = Get-L2State 'L2state before'
$jBefore  = Get-FeatJson 'json before'
Say "L2state(before) = $(if ($null -eq $l2Before) { 'MISSING' } else { $l2Before })"

Head "BENCH (-passes $Passes)"
Say "running: $bench `"$canon`" -passes $Passes"
$swBench = [Diagnostics.Stopwatch]::StartNew()
$benchOut = & $bench @($canon, '-passes', "$Passes") 2>&1
$benchRc = $LASTEXITCODE
$swBench.Stop()
$benchText = ($benchOut | Out-String)
foreach ($l in $benchOut) { Say "  $l" }
Say ("bench rc={0}  wall={1:N1}s" -f $benchRc, $swBench.Elapsed.TotalSeconds) `
    $(if ($benchRc -eq 0) { 'ok' } else { 'FAIL' })
Write-LogLine ("RAW[bench] {0}" -f ($benchText.Trim() -replace "`r?`n", ' | '))

Head "POST-SAMPLES"
$l2After  = Get-L2State 'L2state after'
$jAfter   = Get-FeatJson 'json after'
Say "L2state(after) = $(if ($null -eq $l2After) { 'MISSING' } else { $l2After })"

# --------------------------------------------------- parse bench's own numbers
$bm = @{}
foreach ($kv in @('firstMbps','firstSec','secondMbps','secondSec','bytes','secondHitRate','driverCold','ioMode')) {
    if ($benchText -match ($kv + '=("?[^"\s]+"?)')) { $bm[$kv] = $Matches[1].Trim('"') }
}
Head "BENCH STRUCTURED LINE (verbatim)"
Say ("raw: {0}" -f ($benchText.Trim() -replace "`r?`n", ' | '))
foreach ($k in @('firstMbps','firstSec','secondMbps','secondSec','bytes','secondHitRate','driverCold','ioMode')) {
    Say ("  {0} = {1}" -f $k, $(if ($bm.ContainsKey($k)) { $bm[$k] } else { 'MISSING' }))
}

# ================================================================== verdict
# Structure (hard order -- a later tier may NOT be evaluated if an earlier one
# failed):  G0a passes==2  G0 bench rc  G1 driverCold  G2 bench bytes  G3 L2state
#           G4 counter integers  G5 secondHitRate  ->  N2 byte gate (model)
#           ->  r2 bands (D-01 verdict).
# N2 is the MODEL gate because the byte side is independent of any coalescing
# assumption (coalescing changes interval COUNT, never BYTE COUNT). r2 is the
# thing under test, so it can never also be the model gate -- that was the
# defect in the previous version (a r2=131072 run produced "COUNTER-OUT-OF-MODEL"
# AND "D01-REFUTED" simultaneously, which would have thrown away the one
# conclusion that mattered most).
Head "VERDICT (gate chain -> N2 model gate -> r2 bands)"
$state     = 'MISSING'
$hardFail  = $false
$rc        = 5
$notes     = New-Object System.Collections.Generic.List[string]
$h = $null; $m1 = $null; $m2 = $null; $r2 = $null; $denom = $null
$dMisses = $null; $dBytesMiss = $null
$mean1 = $null; $mean2 = $null; $n2dev = $null

# G5 / G1 / G2 inputs, read verbatim from bench's own line. Never synthesised:
# a missing field stays $null and fails its gate rather than defaulting to 0.
$hStr     = if ($bm.ContainsKey('secondHitRate')) { [string]$bm['secondHitRate'] } else { $null }
$coldStr  = if ($bm.ContainsKey('driverCold'))    { [string]$bm['driverCold'] }    else { $null }
$bytesStr = if ($bm.ContainsKey('bytes'))         { [string]$bm['bytes'] }         else { $null }

if ($Passes -ne 2) {
    # Unreachable now that the red line rejects any other value before elevation.
    # Kept as a belt-and-braces check: if someone relaxes the red line, this gate
    # still refuses to produce a number from a mixture.
    $state = 'INVALID'; $hardFail = $true
    Say ("G0a passes    : FAIL (-Passes {0} != 2) -> INVALID" -f $Passes) 'FAIL'
    $notes.Add("Passes=${Passes}: m2=B*(1-h) is derived for exactly 2 passes; with N>2 the hit-rate window covers passes 2..N and r2 would be a mixture")
} elseif ($benchRc -ne 0) {
    $state = 'INVALID'; $hardFail = $true
    Say ("G0  bench rc  : FAIL (rc={0}) -> INVALID" -f $benchRc) 'FAIL'
    $notes.Add("bench rc=$benchRc -> no second-pass measurement; nothing can be judged")
} elseif ($null -eq $coldStr) {
    $state = 'INVALID'; $hardFail = $true
    Say "G1  driverCold: MISSING from bench line -> INVALID (cannot prove pass 1 was cold)" 'FAIL'
    $notes.Add("driverCold absent: bench did not print it, so m1=B (pass 1 misses everything) is unproven")
} elseif ($coldStr -ne 'true') {
    $state = 'INVALID'; $hardFail = $true
    Say ("G1  driverCold: FAIL ('{0}') -> INVALID" -f $coldStr) 'FAIL'
    $notes.Add("driverCold='$coldStr': CliFlushFileRange failed, so pass 1 was NOT a cold read; m1=B is false and the whole algebra collapses")
} elseif ($null -eq $bytesStr) {
    $state = 'INVALID'; $hardFail = $true
    Say "G2  bench bytes: MISSING from bench line -> INVALID" 'FAIL'
    $notes.Add("bench did not print bytes=; m1/m2 cannot be anchored to what was actually read")
} else {
    $benchBytes = [int64]$bytesStr
    if ($benchBytes -ne $B) {
        $state = 'INVALID'; $hardFail = $true
        Say ("G2  bench bytes: FAIL ({0:N0} != file B {1:N0}) -> INVALID" -f $benchBytes, $B) 'FAIL'
        $notes.Add("bench read $benchBytes B but the file is $B B; the per-slice algebra would be applied to the wrong length")
    } elseif ($null -eq $l2Before -or $null -eq $l2After) {
        $state = 'MISSING'
        Say "G3  L2state   : MISSING (unreadable) -> MISSING (not 0)" 'WARN'
        $notes.Add("L2state unreadable at one or both samples -> gate not satisfiable; MISSING (never treated as 0)")
    } elseif (($l2Before -ne 0) -or ($l2After -ne 0)) {
        $state = 'INVALID'; $hardFail = $true
        Say ("G3  L2state   : FAIL ({0} / {1}) -> INVALID" -f $l2Before, $l2After) 'FAIL'
        $notes.Add("L2state=$l2Before/$l2After must be 0: with L2 serving hits the disk read is skipped, so dMisses no longer counts disk I/O")
    } elseif (-not ($jBefore.Ok -and $jAfter.Ok)) {
        $state = 'MISSING'
        Say "G4  counters  : MISSING (miss/bm unreadable at one or both samples)" 'WARN'
        $notes.Add("miss/bm unreadable -> MISSING (never 0): a zero-filled counter and an unreadable counter must not look alike")
    } elseif ($null -eq $hStr) {
        $state = 'INVALID'; $hardFail = $true
        Say "G5  secondHitRate: MISSING from bench line -> INVALID" 'FAIL'
        $notes.Add("bench did not print secondHitRate; m2 = B*(1-h) has no input and must not be guessed")
    } elseif ([double]$hStr -lt 0) {
        $state = 'INVALID'; $hardFail = $true
        Say ("G5  secondHitRate: FAIL ({0} < 0 = driver offline) -> INVALID" -f $hStr) 'FAIL'
        $notes.Add("secondHitRate=$hStr means UNMEASURABLE (ctl_bench.c:290 keeps -1 rather than faking 0); treating it as h=0 would invent a 32x miss-byte count")
    } else {
        $h = [double]$hStr
        Say "G0a passes    : 2 (1 cold + 1 hot)" 'ok'
        Say "G0  bench rc  : 0" 'ok'
        Say "G1  driverCold: true (flush of the file's driver ranges succeeded -> pass 1 was cold)" 'ok'
        Say ("G2  bench bytes: {0:N0} == file B" -f $benchBytes) 'ok'
        Say ("G3  L2state   : 0 at both samples ({0} / {1})" -f $l2Before, $l2After) 'ok'
        Say "G4  counters  : miss + bm readable at both samples" 'ok'
        Say ("G5  secondHitRate: {0:N4}  (window = pass 2 only: rb_get_stats s0 at ctl_bench.c:408 after pass 1, s1 at :420 after pass 2)" -f $h) 'ok'

        # ---- the four raw counters, full integers, straight off the wire
        Head "RAW COUNTERS (integers, no unit conversion)"
        Say ("before.miss  = {0}" -f $jBefore.Misses)
        Say ("after.miss   = {0}" -f $jAfter.Misses)
        Say ("before.bm    = {0}" -f $jBefore.BytesMiss)
        Say ("after.bm     = {0}" -f $jAfter.BytesMiss)
        Say ("dMisses      = {0}  (= after.miss - before.miss)" -f ($jAfter.Misses - $jBefore.Misses))
        Say ("dBytesMiss   = {0}  (= after.bm - before.bm)" -f ($jAfter.BytesMiss - $jBefore.BytesMiss))

        $dMisses    = [int64]$jAfter.Misses    - [int64]$jBefore.Misses
        $dBytesMiss = [int64]$jAfter.BytesMiss - [int64]$jBefore.BytesMiss
        $m1 = [int64]$B                                  # pass 1: every byte missed, 1 interval per slice
        $m2 = [int64][math]::Round($B * (1.0 - $h))      # pass 2 miss bytes
        $denom = [int64]$dMisses - $S

        # ---- six intermediates, one per line, so a reader can redo the arithmetic
        Head "INTERMEDIATES (one per line)"
        Say ("B  = file bytes            = {0}" -f $B)
        Say ("S  = ceil(B/131072)        = {0}" -f $S)
        Say ("h  = secondHitRate         = {0:N4}" -f $h)
        Say ("m1 = B                     = {0}  (pass 1 miss bytes)" -f $m1)
        Say ("m2 = B*(1-h)               = {0}  (pass 2 miss bytes)" -f $m2)
        Say ("dMisses - S                = {0}  (intervals attributable to pass 2)" -f $denom)
        if ($denom -gt 0) {
            $r2 = $m2 / [double]$denom
            Say ("r2 = m2/(dMisses-S)        = {0:N1}  (pass-2 mean interval bytes)" -f $r2)
        } else {
            Say ("r2 = m2/(dMisses-S)        = UNDEFINED (denominator {0} <= 0)" -f $denom)
        }
        Say ("N2 expected = B*(2-h)      = {0}" -f [int64][math]::Round($B * (2.0 - $h)))

        # ---- tier 1: N2, the model gate. Independent of coalescing.
        Head "N2 MODEL GATE (byte side; independent of any coalescing assumption)"
        $n2ok = $false
        if ($dBytesMiss -le 0) {
            Say ("N2  dBytesMiss = {0} <= 0 -> UNTESTABLE (MISSING, never PASS)" -f $dBytesMiss) 'WARN'
            $notes.Add("N2 untestable: dBytesMiss=$dBytesMiss, so the byte side cannot confirm or refute the two-pass model")
            $state = 'MISSING'
        } else {
            $n2expect = $B * (2.0 - $h)
            $n2dev = [math]::Abs($dBytesMiss - $n2expect) / $dBytesMiss
            if ($n2dev -lt 0.02) {
                $n2ok = $true
                Say ("N2  |{0} - {1}| / {0} = {2:P4} < 2% -> BYTES-CONSISTENT" -f $dBytesMiss, [int64][math]::Round($n2expect), $n2dev) 'ok'
            } else {
                Say ("N2  |{0} - {1}| / {0} = {2:P4} >= 2% -> BYTES-INCONSISTENT" -f $dBytesMiss, [int64][math]::Round($n2expect), $n2dev) 'FAIL'
                $state = 'INVALID'; $hardFail = $true
                $notes.Add("N2: byte-side deviation $('{0:P2}' -f $n2dev) >= 2% -> read bypass, tail-buffered degradation, or counter misalignment. The model is wrong, so r2 is NOT reported as a D-01 answer")
            }
        }

        # ---- tier 2: r2 bands, ONLY if the model gate passed
        Head "D-01 JUDGEMENT (r2 bands; evaluated only if N2 passed)"
        if (-not $n2ok) {
            Say "r2 bands      : NOT EVALUATED (N2 did not pass -> no D-01 conclusion may be drawn)" 'WARN'
        } elseif ($denom -le 0) {
            $state = 'INVALID'; $hardFail = $true
            Say ("r2 bands      : INVALID (dMisses-S = {0} <= 0 -> pass 2 contributed no intervals)" -f $denom) 'FAIL'
            $notes.Add("dMisses=$dMisses <= S=${S}: the model says pass 2 must add at least 1 interval per 128 KiB it missed, so the counter is inconsistent with the model")
        } else {
            $mean1 = 131072.0                  # pass 1: one coalesced interval per 128 KiB slice
            $mean2 = $r2
            # Resolution disclosure: r2's noise is set by the denominator (a count),
            # so REFUTED (denominator ~32x smaller) is structurally weaker evidence
            # than CONFIRMED. Model: count-like 1/sqrt(N). Not a measurement.
            $noisePct = if ($denom -gt 0) { 100.0 / [math]::Sqrt([double]$denom) } else { [double]::NaN }
            Say ("mean interval pass 1 : {0:N0} B   (expected 131,072 = 128 KiB; 1 interval per slice)" -f $mean1)
            Say ("mean interval pass 2 : {0:N1} B   (solved)" -f $mean2)
            Say ("pass1/pass2 ratio    : {0:N1}x  (the 32x gap IS the finding: 32x = fragmented, 1x = clustered)" -f ($mean1 / [math]::Max($mean2, 1)))
            Say ("r2 resolution        : dMisses-S = {0} -> r2 relative noise ~{1:N2}% (count-like 1/sqrt(N); CONFIRMED band ~0.11%, REFUTED band ~0.62%)" -f $denom, $noisePct) 'warn'
            Say ("                       REFUTED is therefore ~6x noisier than CONFIRMED; its band is widened to +-10% for that reason, not to make it easier to pass.") 'warn'

            $lo4k  = 3768.0;   $hi4k  = 4424.0        # 4096  * [0.92, 1.08]
            $lo128 = 117965.0; $hi128 = 144179.0      # 131072 * [0.90, 1.10]
            # The five branches below are a COMPLETE, DISJOINT partition of the
            # real line, and order matters: they must be checked as written,
            # because the physical ceiling (131072) sits INSIDE the REFUTED band.
            #
            #   r2 < lo4k            -> MORE-FRAGMENTED   (sub-4KiB: 512/1K/2K)
            #   [lo4k, hi4k]         -> CONFIRMED
            #   (hi4k, lo128)        -> INCONCLUSIVE
            #   [lo128, hi128]       -> REFUTED
            #   r2 > hi128           -> INVALID
            #
            # A first version put "r2 > 131072 => INVALID" FIRST, which made the
            # upper half of the REFUTED band (131072..144179) dead code and
            # reported a legitimate 131072.4 measurement as instrument failure.
            # Its own edge test caught it. The +-10% on the REFUTED side exists
            # precisely to absorb noise that can push a true 131072 mean slightly
            # above the physical ceiling, so the INVALID bound must be hi128, not
            # 131072. There is now exactly one boundary between REFUTED and
            # INVALID, and no gap.
            if ($r2 -ge $lo4k -and $r2 -le $hi4k) {
                $state = 'D01-CONFIRMED'
                Say ("r2 band       : {0:N1} in [3768, 4424] (4096 +-8%) -> D01-CONFIRMED" -f $r2) 'ok'
                Say "                 pass 2 issues 4 KiB fragmented disk reads; D-01's premise holds" 'ok'
            } elseif ($r2 -ge $lo128 -and $r2 -le $hi128) {
                $state = 'D01-REFUTED'; $hardFail = $true
                Say ("r2 band       : {0:N1} in [117965, 144179] (131072 +-10%) -> D01-REFUTED" -f $r2) 'FAIL'
                Say "                 pass 2 ALSO coalesced into 128 KiB: misses are clustered, candidate D would buy nothing" 'FAIL'
                if ($r2 -gt 131072.0) {
                    Say "                 note: r2 exceeds the 131072 physical ceiling; that excess is noise, not physics." 'warn'
                    Say "                 The band's upper edge is wider than the ceiling for this reason alone." 'warn'
                }
            } elseif ($r2 -gt $hi4k -and $r2 -lt $lo128) {
                $state = 'D01-INCONCLUSIVE'
                Say ("r2 band       : {0:N1} in ({1:N0}, {2:N0}) -> D01-INCONCLUSIVE" -f $r2, $hi4k, $lo128) 'WARN'
                Say "                 neither band. Physically plausible: part of pass 2 fragmented, part clustered." 'WARN'
                Say "                 This is NOT 'model broken' and NOT 'counters unreadable' -- the data is usable," 'WARN'
                Say "                 it simply does not support either conclusion. A per-region breakdown is needed." 'WARN'
                $notes.Add("r2=$('{0:N1}' -f $r2) sits between the bands: a mixed state. Neither D-01-CONFIRMED nor D01-REFUTED may be claimed")
            } elseif ($r2 -gt $hi128) {
                $state = 'INVALID'; $hardFail = $true
                Say ("r2 band       : {0:N1} > {1:N0} -> INVALID" -f $r2, $hi128) 'FAIL'
                Say "                 Even after the band's noise allowance, a mean interval above 144179 B is" 'FAIL'
                Say "                 impossible: RbFeatOnMiss covers at most RB_READ_PLAN_MAX_BYTES=131072" 'FAIL'
                Say "                 (read_plan.h:29). The algebra or a counter is broken. No D-01 conclusion." 'FAIL'
                $notes.Add("r2=$('{0:N1}' -f $r2) is above 144179, i.e. beyond even the noise allowance for the 128 KiB slice ceiling; the decomposition does not hold")
            } else {
                # r2 < 3768: MORE fragmented than 4 KiB. Possible (512 B, 1 KiB,
                # 2 KiB reads). Supports D-01's premise at least as strongly as
                # CONFIRMED does, so it must never be reported as INVALID.
                $state = 'D01-CONFIRMED-MORE-FRAGMENTED'
                Say ("r2 band       : {0:N1} < 3768 (below 4096 -8%) -> D01-CONFIRMED-MORE-FRAGMENTED" -f $r2) 'ok'
                Say "                 pass 2 is MORE fragmented than the 4 KiB D-01 assumed; the premise holds" 'ok'
                Say "                 with margin. Possible causes: sub-4KiB (512/1K/2K) reads, or interval" 'ok'
                Say "                 bookkeeping at less than 4 KiB granularity." 'ok'
                $notes.Add("r2=$('{0:N1}' -f $r2) is below the 4 KiB band: fragmentation is worse than D-01's premise, not better. Premise supported; do NOT treat as instrument failure")
            }
        }
    }
}

Say ""
Say "state         : $state"
foreach ($n in $notes) { Say "note: $n" 'WARN' }

Head "VERDICT SUMMARY"
Say ("dMisses      = {0}" -f $(if ($null -eq $dMisses) { 'MISSING' } else { '{0}' -f $dMisses }))
Say ("dBytesMiss   = {0}" -f $(if ($null -eq $dBytesMiss) { 'MISSING' } else { '{0} B' -f $dBytesMiss }))
Say ("B            = {0}" -f $B)
Say ("S            = {0}" -f $S)
Say ("h            = {0}" -f $(if ($null -eq $h) { 'MISSING' } else { '{0:N4}' -f $h }))
Say ("m1           = {0}" -f $(if ($null -eq $m1) { 'MISSING' } else { '{0} B' -f $m1 }))
Say ("m2           = {0}" -f $(if ($null -eq $m2) { 'MISSING' } else { '{0} B' -f $m2 }))
Say ("dMisses-S    = {0}" -f $(if ($null -eq $denom) { 'MISSING' } else { '{0}' -f $denom }))
Say ("r2           = {0}" -f $(if ($null -eq $r2) { 'MISSING' } else { '{0:N1} B' -f $r2 }))
Say ("mean pass1   = {0}" -f $(if ($null -eq $mean1) { 'MISSING' } else { '{0:N0} B' -f $mean1 }))
Say ("mean pass2   = {0}" -f $(if ($null -eq $mean2) { 'MISSING' } else { '{0:N0} B' -f $mean2 }))
Say ("N2 dev       = {0}" -f $(if ($null -eq $n2dev) { 'MISSING' } else { '{0:P4}' -f $n2dev }))
Say ("state        = {0}" -f $state)
Say ""
Say "BOUNDARY (what this run does NOT establish):" 'warn'
Say "  * r2 is a MEAN over pass 2, not an interval-size distribution." 'warn'
Say "  * D01-CONFIRMED says D-01's premise holds; it does NOT say candidate D" 'warn'
Say "    yields 4x. That needs the QD curve (D-01 section 7 P1)." 'warn'
Say "  * The OS file cache is not controlled by bench (ioMode ends in" 'warn'
Say "    +os-cache-unknown); only the driver-side range is flushed." 'warn'
Say "  * Nothing here touches the 0x7F write path -- that is RB-write-load.ps1." 'warn'
Say "  * Single run, single file, single machine. Not a distribution of runs." 'warn'

Say ("total wall = {0:N1} s" -f ((Get-Date) - $t0All).TotalSeconds)

# rc map (each code means exactly one thing):
#   0 = D01-CONFIRMED (or CONFIRMED-MORE-FRAGMENTED): the D-01 premise holds
#   3 = D01-REFUTED, or INVALID (a hard gate or the model itself broke)
#   4 = red line / abort (set inside Fail-Early or trap)
#   5 = MISSING: a required counter or field was unreadable -> run unusable
#   6 = D01-INCONCLUSIVE: usable data, in neither band; not 3 and not 5
if ($state -eq 'MISSING')                        { $rc = 5 }
elseif ($state -eq 'D01-INCONCLUSIVE')           { $rc = 6 }
elseif ($hardFail)                               { $rc = 3 }
else                                             { $rc = 0 }

Write-LogLine ("TERMINAL rc={0} state={1} dMisses={2} dBytesMiss={3} B={4} S={5} h={6} m1={7} m2={8} denom={9} r2={10} N2dev={11} passes={12}" -f `
    $rc, $state, `
    $(if ($null -eq $dMisses) { 'MISSING' } else { $dMisses }), `
    $(if ($null -eq $dBytesMiss) { 'MISSING' } else { $dBytesMiss }), `
    $B, $S, `
    $(if ($null -eq $h) { 'MISSING' } else { '{0:N4}' -f $h }), `
    $(if ($null -eq $m1) { 'MISSING' } else { $m1 }), `
    $(if ($null -eq $m2) { 'MISSING' } else { $m2 }), `
    $(if ($null -eq $denom) { 'MISSING' } else { $denom }), `
    $(if ($null -eq $r2) { 'MISSING' } else { [math]::Round($r2, 2) }), `
    $(if ($null -eq $n2dev) { 'MISSING' } else { '{0:F6}' -f $n2dev }), `
    $Passes)
if ($LogPath) { Say "log appended: $LogPath" }
exit $rc