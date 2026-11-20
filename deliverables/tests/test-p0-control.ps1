# test-p0-control.ps1  [JUDGE SELFTEST - keep: this is the proof that the criteria have teeth]
# WHAT IT PROVES: the shipped Get-P0State: every one of the five P0 states is reachable, and an unreadable counter reaches UNREADABLE instead of a pass
# Extracts the shipped P0 control block from RB-write-load.ps1 and evaluates it
# with synthetic samples, so all four documented states are reachable:
#   UNREADABLE / ZERO-AS-EXPECTED / LOW-AS-EXPECTED / HIGH-UNEXPECTED
# It also checks the boundary arithmetic (anchor 14,400 vs gate 28,800) and the
# L2 / N2 sub-cases. No device access, no elevation, no D: traffic.
#
# Usage:  test-p0-control.ps1 -Case <name> [-Show]
# The rc-style contract: exit 0 = the state matched the expectation,
# 1 = state mismatch (a real failure worth reading), 90/92 = harness fault.

param(
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) 'RB-write-load.ps1'),
    [string]$Case = 'ZERO',
    [string]$Expect = '',
    [switch]$Show
)

$ErrorActionPreference = 'Stop'
$text = [IO.File]::ReadAllText($Source)

# The state machine now lives in a PURE FUNCTION (Get-P0State) so the replay
# harness can slice it instead of rewriting it. This harness therefore needs TWO
# slices: the function itself, and the block that calls it and prints.
$fs = $text.IndexOf('function Get-P0State')
$fe = $text.IndexOf('function Write-RealFile', $fs)
if ($fs -lt 0 -or $fe -le $fs) { Write-Host 'SELFTEST FAIL: cannot locate Get-P0State'; exit 90 }
$fnSlice = $text.Substring($fs, $fe - $fs)

$startMark = '# ---- P0 after-sample + MISS COUNTER READ-PATH-ONLY CONTROL'
$endMark   = 'Say "P0 state      : $p0state"'
$i = $text.IndexOf($startMark)
$j = $text.IndexOf($endMark)
if ($i -lt 0 -or $j -lt $i) { Write-Host 'SELFTEST FAIL: cannot locate the P0 control block'; exit 90 }
$block = $text.Substring($i, ($j - $i) + $endMark.Length)

# Run the shipped function definition so the block below can call it. If this
# fails, the block would throw "Get-P0State is not recognized" and every case
# would report a bogus state -- which is exactly what happened when the state
# machine was first extracted and this harness still sliced only the block.
try { Invoke-Expression $fnSlice } catch { Write-Host "SELFTEST FAIL: Get-P0State slice: $($_.Exception.Message)"; exit 90 }
if (-not (Get-Command Get-P0State -ErrorAction SilentlyContinue)) {
    Write-Host 'SELFTEST FAIL: Get-P0State did not get defined by the slice'; exit 90
}

$script:lines = New-Object System.Collections.Generic.List[string]
function Head([string]$m) { $null = $script:lines.Add("=== $m ===") }
function Say([string]$m, [string]$tag = '') { $null = $script:lines.Add($m) }
function Dump-Lines { if ($Show) { foreach ($l in $script:lines) { Write-Output $l } } }

function New-Sample([int64]$miss, [int64]$bm, [int64]$l2, [bool]$ok) {
    [pscustomobject]@{ Label = 'x'; Hits = $null; Misses = $miss; BytesMiss = $bm
                       L2State = $l2; Flags = 0; Ok = $ok }
}

$Rounds = 2
$SizeMB = 900
$feat = 'unused'

# The shipped block calls Get-FeatSample 'P0 after' itself, so the after-sample
# must come from the stub FUNCTION, not from a pre-set variable (the block would
# overwrite it). Counters are CUMULATIVE, so 'before' must be non-zero: a zero
# baseline makes dMisses equal the raw counter and would pass cases for the
# wrong reason.
$BASE = [int64]500000
$p0Before = New-Sample $BASE ([int64]1000000) 0 $true
$script:afterSample = New-Sample $BASE ([int64]1000000) 0 $true
function Get-FeatSample([string]$label) { return $script:afterSample }

# The shipped block reads Get-CounterSnapshot $postStatus to get ReadReq, so the
# stub must return a snapshot with a controllable ReadReq.
$script:postStatus = 'stub status'
$script:preReadReq  = [int64]331322
$script:postReadReq = [int64]331322
function Get-CounterSnapshot([string]$txt) {
    [pscustomobject]@{ WriteBytes = $null; WriteBytesLine = $null; ReadBytes = $null
                       WriteReq = $null; ReadReq = $script:postReadReq
                       ReadSource = 'stub'; Raw = $txt }
}
# $snapPre is referenced by the shipped block for the read-request delta.
$snapPre = [pscustomobject]@{ WriteBytes = $null; WriteBytesLine = $null; ReadBytes = $null
                              WriteReq = $null; ReadReq = [int64]331322
                              ReadSource = 'stub'; Raw = $null }

# BytesMiss tracks the miss delta at 4096 B/interval wherever N2 is meant to
# pass, so the N2 line does not accidentally produce a FAIL in unrelated cases.
$bm = [int64]1000000
switch ($Case) {
    # --- the five documented states ---------------------------------------
    'ZERO'      { $script:afterSample = New-Sample ($BASE + 0)     $bm 0 $true; $Expect = if ($Expect) { $Expect } else { 'ZERO-UNINFORMATIVE' } }
    'ZEROREAD'  { $script:afterSample = New-Sample ($BASE + 0)     $bm 0 $true
                  $script:postReadReq = [int64]400000; $Expect = if ($Expect) { $Expect } else { 'ZERO-AS-EXPECTED' } }
    'LOW1'      { $script:afterSample = New-Sample ($BASE + 1)     $bm 0 $true; $Expect = if ($Expect) { $Expect } else { 'LOW-AS-EXPECTED' } }
    'ANCHOR'    { $script:afterSample = New-Sample ($BASE + 14400) $bm 0 $true; $Expect = if ($Expect) { $Expect } else { 'LOW-AS-EXPECTED' } }
    'ANCHORP1'  { $script:afterSample = New-Sample ($BASE + 14401) $bm 0 $true; $Expect = if ($Expect) { $Expect } else { 'LOW-AS-EXPECTED' } }
    'GATE'      { $script:afterSample = New-Sample ($BASE + 28800) $bm 0 $true; $Expect = if ($Expect) { $Expect } else { 'LOW-AS-EXPECTED' } }
    'GATEP1'    { $script:afterSample = New-Sample ($BASE + 28801) $bm 0 $true; $Expect = if ($Expect) { $Expect } else { 'HIGH-UNEXPECTED' } }
    'BIGLEAK'   { $script:afterSample = New-Sample ($BASE + 989988) $bm 0 $true; $Expect = if ($Expect) { $Expect } else { 'HIGH-UNEXPECTED' } }
    'UNREAD'    { $script:afterSample = New-Sample 0 0 0 $false;                $Expect = if ($Expect) { $Expect } else { 'UNREADABLE' } }
    'L2ON'      { $script:afterSample = New-Sample ($BASE + 10) $bm 0 $true
                  $p0Before = New-Sample $BASE $bm 2 $true;                     $Expect = if ($Expect) { $Expect } else { 'UNREADABLE' } }
    # ReadReq unreadable (null) must NOT be read as "reads happened".
    'ZERONOREQ' { $script:afterSample = New-Sample ($BASE + 0)     $bm 0 $true
                  $script:postReadReq = $null;                                  $Expect = if ($Expect) { $Expect } else { 'ZERO-UNINFORMATIVE' } }
    # --- sub-judgements that must not change the state --------------------
    # dMisses=1000 is > 0, so these land in LOW; N2 is reported inside it and
    # must not promote or demote the state on its own.
    'N2BAD'     { $script:afterSample = New-Sample ($BASE + 1000) ([int64]9000000) 0 $true; $Expect = if ($Expect) { $Expect } else { 'LOW-AS-EXPECTED' } }
    'N2ZERO'    { $script:afterSample = New-Sample ($BASE + 1000) ([int64]1000000) 0 $true; $Expect = if ($Expect) { $Expect } else { 'LOW-AS-EXPECTED' } }
    default     { Write-Host "SELFTEST unknown case '$Case'"; exit 90 }
}

$probe = $block + "`nDump-Lines; Write-Output ('SELFTEST_STATE=' + `$p0state)"
try { Invoke-Expression $probe } catch { Write-Host "SELFTEST FAIL: $($_.Exception.Message)"; exit 92 }

# The assertion lives HERE, not in the driver: the driver could otherwise print
# a PASS for a state it merely echoed. exit 0 = state matched, 1 = mismatch.
if (-not $Expect) { Write-Host 'SELFTEST FAIL: no expectation supplied'; exit 90 }
if ($p0state -eq $Expect) { exit 0 }
Write-Host ("EXPECT-MISMATCH: want={0} got={1}" -f $Expect, $p0state)
exit 1
