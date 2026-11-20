# test-replay-real-run.ps1  [JUDGE SELFTEST - keep: this is the proof that the criteria have teeth]
# WHAT IT PROVES: the shipped P4 and P0 judges, fed the numbers a real run
# produced, return PASS for the correct denominator and FAIL for the old wrong
# one -- and that the P0 state machine lands on ZERO-UNINFORMATIVE, not
# ZERO-AS-EXPECTED.
#
# WHAT THIS PROVES: that the SHIPPED judge in RB-write-load.ps1, fed the numbers a
# real run actually produced, returns PASS for the correct denominator and FAIL
# for the old wrong one -- and that the P0 state machine, sliced out of the same
# shipped file, lands on ZERO-UNINFORMATIVE rather than ZERO-AS-EXPECTED.
#
# HISTORICAL INPUT -- READ THIS BEFORE TRUSTING THE OUTPUT
# -------------------------------------------------------
# The real run was 2026-11-20 15:18:40, rc=0, 487 log lines. That log was
# destroyed by a test harness Remove-Item glob. 67 of the 487 lines were salvaged
# from the assistant session transcript and are stored alongside this file as
# _SALVAGED-151840-fragments.txt. Everything below is quoted from that salvage or
# from the run's own TERMINAL line, and each figure names its source line.
#
# The salvaged set is a SPARSE SAMPLE, not a contiguous log: 420 lines are gone,
# including every [WARN] and every per-assertion VDL/reclaim line. So this replay
# can re-check "what the numbers were" and CANNOT re-check "whether the run
# emitted a silent warning".
#
# CONTRACT (hardened 2026-11-20):
#   0  = every replayed number matches its independently-recorded expected value,
#        AND the 67 salvaged lines only. The pass line itself says so; see the
#        exit block at the bottom of this file.
#   2  = THE INPUT IS GONE. This is a hard failure, never a pass. An earlier
#        version fell back to two hard-coded lines and still printed
#        "REPLAY OK ... EXIT=0" -- i.e. it verified its own constants and called
#        that proof. That is the "empty set passes" family, one level up, in the
#        exact file whose job was to prove the judge has teeth.
#   1  = a replayed number disagrees with its expected value
#
# SCOPE OF WHAT A PASS HERE MEANS -- read before citing it:
# 2026-11-20: the ONLY standing input is _SALVAGED-151840-fragments.txt (67 of 487
# lines). Any run of this harness proves the numbers in those 67 lines, and nothing
# about the 420 missing lines. Do not describe its output as "replayed the real
# run".
#
# Expected values are kept SEPARATE from the parsed input on purpose. Where an
# expected value could not be sourced independently, the assertion was DELETED
# rather than left as a tautology (see the note where "~4 requests/MB" used to be).

param(
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) 'RB-write-load.ps1'),
    [string]$Salvage = (Join-Path $PSScriptRoot '_SALVAGED-151840-fragments.txt'),
    [string]$Log = 'C:\Users\陈\AppData\Local\Temp\RB-write-load_20261120_151840.log'
)

$ErrorActionPreference = 'Stop'

function Fail-Hard([string]$why) {
    Write-Output ('REPLAY FAILED: ' + $why)
    Write-Output '               this run proves nothing about the shipped judge'
    exit 2
}

# ---- locate the input: the real log if it still exists, else the salvage -----
# The real log is gone in practice; the salvage is the standing input. Either one
# must exist -- a missing input is a hard failure, never a quiet fallback.
if (Test-Path -LiteralPath $Log) {
    $inputPath = $Log
    $inputKind = 'ORIGINAL LOG'
} elseif (Test-Path -LiteralPath $Salvage) {
    $inputPath = $Salvage
    $inputKind = 'SALVAGED FRAGMENTS (67 of 487 lines)'
} else {
    Fail-Hard 'the real log is gone AND the salvage file is missing; no historical input available'
}

# ---- slice the SHIPPED code -----------------------------------------------
# Both judges are pure functions in the shipped file precisely so this harness can
# run those bytes instead of a rewritten copy. P0 used to be inline, which forced
# a second implementation here -- verifying the intent, not the artifact.
# ONE contiguous slice from Get-CounterSnapshot up to Write-RealFile covers the
# counter parser, Get-P4RatioState, Format-P4RatioNote and Get-P0State, because
# they are defined in that order with nothing else interleaved. Slicing only from
# Get-P4RatioState would leave Get-CounterSnapshot undefined (it is defined
# earlier in the file) -- the first run of this version died exactly there.
$text = [IO.File]::ReadAllText($Source)
$sliceStart = $text.IndexOf('function Get-CounterSnapshot')
$sliceEnd   = $text.IndexOf('function Write-RealFile')
if ($sliceStart -lt 0 -or $sliceEnd -lt 0 -or $sliceEnd -le $sliceStart) {
    Fail-Hard 'cannot locate the counter/judge function block in the shipped file'
}
# The P0 state machine must be inside that block: if someone inlines it again,
# this harness would silently go back to having no shipped-code evidence for P0.
if ($text.IndexOf('function Get-P0State') -lt 0) {
    Fail-Hard 'Get-P0State is missing from the shipped file (the P0 state machine must stay a sliceable function)'
}
Invoke-Expression $text.Substring($sliceStart, $sliceEnd - $sliceStart)
foreach ($fn in @('Get-CounterSnapshot', 'Format-CounterDelta', 'Get-P4RatioState', 'Format-P4RatioNote', 'Get-P0State')) {
    if (-not (Get-Command $fn -ErrorAction SilentlyContinue)) {
        Fail-Hard ("the shipped slice did not define {0}; the replay cannot run" -f $fn)
    }
}

$fail = 0
function T([string]$what, [string]$got, [string]$want) {
    if ($got -eq $want) { Write-Output ("  ok    {0,-56} -> {1}" -f $what, $got) }
    else { Write-Output ("  FAIL  {0,-56} -> got {1}, want {2}" -f $what, $got, $want); $script:fail++ }
}

Write-Output "=== input ==="
Write-Output ("  kind : {0}" -f $inputKind)
Write-Output ("  file : {0}" -f $inputPath)

# ---- parse the two counter lines out of the input --------------------------
$all = Get-Content -LiteralPath $inputPath -Encoding UTF8
# Exclude the shipped script's own "raw counter : pre=[..] post=[..]" echo line:
# it carries BOTH readings on one line and the shipped parser takes the first
# match, so feeding it would compare pre against pre. (This harness bug was
# caught by the dWrite assertion below coming out 0 -- the assertions doing their
# job on the harness.)
$wrLines = @($all | Where-Object { $_ -match '写请求:\s*\d+\s+写字节' -and $_ -notmatch 'raw counter|per-round|expected breakdown' } |
                ForEach-Object { $_.Trim() })
if ($wrLines.Count -lt 2) {
    Fail-Hard ("need two 写请求/写字节 lines in the input, found {0} in {1}" -f $wrLines.Count, $inputPath)
}
$preLine  = $wrLines | Select-Object -First 1
$postLine = $wrLines | Select-Object -Last 1
Write-Output ("  pre  line : [{0}]" -f $preLine)
Write-Output ("  post line : [{0}]" -f $postLine)

$pre  = Get-CounterSnapshot $preLine
$post = Get-CounterSnapshot $postLine
Write-Output "=== shipped parser applied to the historical lines ==="
Write-Output ("  pre  WriteBytes={0:N0}  WriteReq={1}" -f $pre.WriteBytes, $pre.WriteReq)
Write-Output ("  post WriteBytes={0:N0}  WriteReq={1}" -f $post.WriteBytes, $post.WriteReq)
if ($null -eq $pre.WriteBytes -or $null -eq $post.WriteBytes) {
    Fail-Hard 'the shipped parser returned no WriteBytes for the historical lines; nothing to judge'
}

$dWrite = Format-CounterDelta $pre $post 'WriteBytes'
$dReq   = $post.WriteReq - $pre.WriteReq
Write-Output ("  dWrite = {0:N2} MB   writeReq delta = {1:N0}" -f ([double]$dWrite/1MB), $dReq)

# ---- EXPECTED VALUES: recorded independently of the parse -------------------
# Source: the run's own TERMINAL / P4 block, quoted in the salvage and preserved
# by hand. These are literals on purpose. Deriving them from $dWrite would make
# every assertion below a tautology, which is the defect this file exists to
# avoid.
$EXPECTED_DWRITE_MB   = 3655.68      # salvage line 466: "dWrite=3,655.68 MB"
$EXPECTED_WRITEREQ    = 13526        # salvage line 469/472 context: "writeReq delta=13,526"
$EXPECTED_RATIO_FULL  = 1.0155       # 3,655.68 / 3,600, computed once by hand
$EXPECTED_RATIO_OLD   = 2.0309       # 3,655.68 / 1,800
$EXPECTED_R1_MB       = 1863.68      # salvage line 472: "R1 dWrite=1,863.68 MB"
$EXPECTED_R2_MB       = 1792.00      # salvage line 473: "R2 dWrite=1,792.00 MB"
$EXPECTED_MISSES      = 228209       # rbfeatctl stats, both samples identical
$EXPECTED_READREQ     = 331322       # 读请求, all 8 salvaged occurrences identical

T 'dWrite reproduces the recorded 3,655.68 MB (+-0.01 GB quantisation)' `
    ("{0:F2}" -f ([double]$dWrite/1MB)) ("{0:F2}" -f $EXPECTED_DWRITE_MB)
T 'writeReq delta reproduces the recorded 13,526' ("{0:N0}" -f $dReq) ("{0:N0}" -f $EXPECTED_WRITEREQ)
# A note on what used to be here: an assertion '~4 requests per MB' whose got and
# want were BOTH derived from the constant 13,526 -- it could not fail for any
# input. Deleted rather than dressed up, because an assertion that cannot fail is
# worse than no assertion: it inflates the count of "checks" without checking.

# The per-round figures must add up to the global one. This is a real cross-check:
# R1 and R2 are two independent recorded numbers, and their sum is compared to the
# global delta parsed from the log. If either were invented this would break.
T 'R1 + R2 (two recorded rounds) sum to the parsed global dWrite' `
    ("{0:F2}" -f ($EXPECTED_R1_MB + $EXPECTED_R2_MB)) ("{0:F2}" -f ([double]$dWrite/1MB))

# ---- the two denominators, and the verdict each produces --------------------
$directOnly = 1800.0 * 1MB      # OLD, wrong: A written only
$full       = 3600.0 * 1MB      # NEW, correct: A written + B copied
Write-Output "=== shipped judge on the historical dWrite ==="
$stOld = Get-P4RatioState $dWrite $directOnly
$stNew = Get-P4RatioState $dWrite $full
Write-Output ("  old denominator 1,800 MB -> ratio {0:P2} -> {1}" -f ([double]$dWrite/$directOnly), $stOld)
Write-Output ("  new denominator 3,600 MB -> ratio {0:P2} -> {1}" -f ([double]$dWrite/$full), $stNew)
Write-Output ("  old note : {0}" -f (Format-P4RatioNote $dWrite $directOnly $stOld))
Write-Output ("  new note : {0}" -f (Format-P4RatioNote $dWrite $full $stNew))
T 'the OLD denominator is now caught as FAIL, not waved through' $stOld 'FAIL'
T 'the CORRECT denominator is PASS' $stNew 'PASS'
T 'measured ratio on the full denominator matches the hand-computed 1.0155' `
    ("{0:F4}" -f ([double]$dWrite/$full)) ("{0:F4}" -f $EXPECTED_RATIO_FULL)
T 'measured ratio on the old denominator matches the hand-computed 2.0309' `
    ("{0:F4}" -f ([double]$dWrite/$directOnly)) ("{0:F4}" -f $EXPECTED_RATIO_OLD)

# ---- P0: run the SHIPPED state machine on the historical counters -----------
# dReadReq = 0 is the whole point: the copies were served from cache, so nothing
# was ever asked to be read. The correct answer is ZERO-UNINFORMATIVE -- "the
# counter did not move, and that tells us nothing about counting reads" -- NOT
# ZERO-AS-EXPECTED, which would dress an unexercised instrument up as a pass.
Write-Output "=== shipped Get-P0State on the historical counters ==="
$dMisses  = [int64]0                                        # 228,209 -> 228,209
$dReadReq = [int64]0                                        # 331,322 -> 331,322
$gate     = [int64]28800                                    # anchor 14,400 x2
Write-Output ("  dMisses={0}  dReadReq={1}  gate={2}  L2state=0/0" -f $dMisses, $dReadReq, $gate)
T 'historical counters land on ZERO-UNINFORMATIVE' `
    (Get-P0State $dMisses $dReadReq 0 0 $gate) 'ZERO-UNINFORMATIVE'
T 'and NOT on ZERO-AS-EXPECTED (the old wrong answer)' `
    $(if ((Get-P0State $dMisses $dReadReq 0 0 $gate) -eq 'ZERO-AS-EXPECTED') { 'REGRESSED' } else { 'correct' }) 'correct'
# Boundary probes through the shipped function, not a rewritten one:
T 'dMisses=0 with real reads observed -> ZERO-AS-EXPECTED' `
    (Get-P0State ([int64]0) ([int64]500) 0 0 $gate) 'ZERO-AS-EXPECTED'
T 'dMisses=1 with no reads -> LOW-AS-EXPECTED' `
    (Get-P0State ([int64]1) ([int64]0) 0 0 $gate) 'LOW-AS-EXPECTED'
T 'dMisses above the gate -> HIGH-UNEXPECTED' `
    (Get-P0State ([int64]28801) ([int64]0) 0 0 $gate) 'HIGH-UNEXPECTED'
T 'L2 on makes the reading UNREADABLE, never a pass' `
    (Get-P0State ([int64]0) ([int64]0) 1 0 $gate) 'UNREADABLE'
T 'unreadable dMisses stays MISSING -> UNREADABLE, never 0' `
    (Get-P0State $null ([int64]0) 0 0 $gate) 'UNREADABLE'

Write-Output ""
if ($fail -eq 0) {
    # The exit line must carry the same scope the :35 header carries. A reader
    # who only runs this file and never opens it sees exactly this string, and
    # "replayed the real run" is the one phrase the header forbids. An earlier
    # version of this project already had a comment criticising
    # "REPLAY OK ... EXIT=0 is self-verification" while the code below it
    # printed plain REPLAY OK -- criticising a failure family and not repeating
    # it are two different things. Keep the boundary IN THE OUTPUT.
    Write-Output 'REPLAY OK - every number above matches the 67 salvaged lines (420 of 487 lines are GONE; this proves the judge arithmetic, not the real run)'
    exit 0
}
Write-Output ("REPLAY FAILED: {0} assertion(s) disagree with the recorded values" -f $fail)
exit 1