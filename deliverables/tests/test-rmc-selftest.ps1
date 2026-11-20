# test-rmc-selftest.ps1  [JUDGE SELFTEST - keep: this is the proof that the criteria have teeth]
# WHAT IT PROVES: the shipped bench verdict core reaches every branch without
# touching D:, UAC or the driver.
#
# Reaches every branch of the judgement core in RB-bench-miss-coalescing.ps1
# WITHOUT touching D:, without UAC, without running bench or the driver.
#
# Method: extract the verdict section from the REAL file (from the
# "==== verdict" marker to EOF) and evaluate it with variables preset. This
# tests the shipped text, not a copy -- a copy would drift and prove nothing.
# The block ends in `exit $rc`, so each case's rc arrives as the process exit
# code and the driver can compare it to the expected code.
#
# Case data is synthetic and arithmetic-consistent: for each intended r2 we
# invert dMisses = S + m2/r2, so the case exercises the intended band rather
# than an arbitrary number.

param(
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) 'RB-bench-miss-coalescing.ps1'),
    [string]$Case = '',
    [switch]$Show      # dump the captured Say lines (what a human would see)
)

$text = [IO.File]::ReadAllText($Source)
# Marker includes the leading '#' so the extracted block starts at a line
# boundary. (First attempt used the bare rule line and cut mid-line, which
# produced a non-terminating "term not recognized" error that the harness
# swallowed -- the exact silent-failure class this project keeps re-hitting.)
$marker = '# ================================================================== verdict'
$idx = $text.IndexOf($marker)
if ($idx -lt 0) { Write-Host 'SELFTEST FAIL: verdict marker not found in source'; exit 90 }
$block = $text.Substring($idx)
$script:seen = New-Object System.Collections.Generic.List[string]

# ---------------------------------------------------------------- UI stubs
$script:lines = New-Object System.Collections.Generic.List[string]
function Head([string]$m) { $null = $script:lines.Add("=== $m ===") }
function Say([string]$m, [string]$tag = '') { $null = $script:lines.Add($m) }
function Write-LogLine([string]$l) { $null = $script:lines.Add("LOG $l") }
function Dump-Lines {
    if (-not $Show) { return }
    foreach ($l in $script:lines) { Write-Output $l }
}
$LogPath = ''
$t0All = Get-Date
$canon = 'D:\synthetic\file.bin'

# ---------------------------------------------------------- fixed physics
$B = [int64]21713462848
$SLICE = [int64]131072
$S = [int64][math]::Ceiling($B / [double]$SLICE)      # 165,661
$H_TRUE = 0.8445
$M2 = [int64][math]::Round($B * (1.0 - $H_TRUE))      # 3,376,443,473
$BYTES_TRUE = [int64][math]::Round($B * (2.0 - $H_TRUE))

function New-Sample([int64]$miss, [int64]$bm, [bool]$ok) {
    [pscustomobject]@{ Label = 'x'; Hits = $null; Misses = $miss; BytesMiss = $bm; Ok = $ok; Raw = 'synthetic' }
}

# dMisses that yields the requested r2, with pass 1's S intervals included.
function Get-MissesFor([double]$r2) {
    [int64]($S + [math]::Round($M2 / $r2))
}

function Set-Case([string]$name) {
    # defaults: a run that should land on D01-CONFIRMED
    $script:benchRc = 0
    $script:Passes = 2
    $script:l2Before = [int64]0
    $script:l2After = [int64]0
    $script:bm = @{
        firstMbps = '1234.5'; firstSec = '16.78'; secondMbps = '10111.0'; secondSec = '2.05'
        bytes = "$B"; secondHitRate = '{0:F4}' -f $H_TRUE; driverCold = 'true'
        ioMode = 'no-buffered+os-cache-unknown'
    }
    $script:jBefore = New-Sample ([int64]1000) ([int64]1048576) $true
    $script:jAfter = New-Sample (1000 + (Get-MissesFor 4096.0)) (1048576 + $BYTES_TRUE) $true

    switch ($name) {
        'CONFIRMED'          { }
        'REFUTED'            { $script:jAfter = New-Sample (1000 + (Get-MissesFor 131072.0)) (1048576 + $BYTES_TRUE) $true }
        'INCONCLUSIVE'       { $script:jAfter = New-Sample (1000 + (Get-MissesFor 32768.0)) (1048576 + $BYTES_TRUE) $true }
        'N2_FAIL'            { $script:jAfter = New-Sample (1000 + (Get-MissesFor 4096.0)) (1048576 + [int64]($BYTES_TRUE * 1.05)) $true }
        'COLD_FALSE'         { $script:bm['driverCold'] = 'false' }
        'COLD_MISSING'       { $null = $script:bm.Remove('driverCold') }
        'HITRATE_NEG'        { $script:bm['secondHitRate'] = '-1.0000' }
        'HITRATE_MISSING'    { $null = $script:bm.Remove('secondHitRate') }
        'BYTES_MISMATCH'     { $script:bm['bytes'] = "$($B - 4096)" }
        'BYTES_MISSING'      { $null = $script:bm.Remove('bytes') }
        'L2_ON'              { $script:l2After = [int64]1 }
        'L2_MISSING'         { $script:l2After = $null }
        'COUNTER_MISSING'    { $script:jAfter = New-Sample ([int64]0) ([int64]0) $false }
        'BENCH_RC'           { $script:benchRc = 1 }
        'PASSES_3'           { $script:Passes = 3 }
        'DENOM_LE_ZERO'      { $script:jAfter = New-Sample (1000 + $S - 661) (1048576 + $BYTES_TRUE) $true }
        'R2_OVER_CEILING'    { $script:jAfter = New-Sample (1000 + $S + 1) (1048576 + $BYTES_TRUE) $true }
        'N2_UNTESTABLE'      { $script:jAfter = New-Sample (1000 + (Get-MissesFor 4096.0)) ([int64]1048576) $true }
        # ---- band edges, one unit either side. Each is labelled by the r2 it
        # targets; Get-MissesFor inverts the algebra so the solved r2 lands there.
        'EDGE_3767'          { $script:jAfter = New-Sample (1000 + (Get-MissesFor 3767.0)) (1048576 + $BYTES_TRUE) $true }
        'EDGE_3769'          { $script:jAfter = New-Sample (1000 + (Get-MissesFor 3769.0)) (1048576 + $BYTES_TRUE) $true }
        'EDGE_4423'          { $script:jAfter = New-Sample (1000 + (Get-MissesFor 4423.0)) (1048576 + $BYTES_TRUE) $true }
        'EDGE_4425'          { $script:jAfter = New-Sample (1000 + (Get-MissesFor 4425.0)) (1048576 + $BYTES_TRUE) $true }
        'EDGE_117964'        { $script:jAfter = New-Sample (1000 + (Get-MissesFor 117964.0)) (1048576 + $BYTES_TRUE) $true }
        'EDGE_117966'        { $script:jAfter = New-Sample (1000 + (Get-MissesFor 117966.0)) (1048576 + $BYTES_TRUE) $true }
        'EDGE_144178'        { $script:jAfter = New-Sample (1000 + (Get-MissesFor 144178.0)) (1048576 + $BYTES_TRUE) $true }
        'EDGE_144180'        { $script:jAfter = New-Sample (1000 + (Get-MissesFor 144180.0)) (1048576 + $BYTES_TRUE) $true }
        'R2_512'             { $script:jAfter = New-Sample (1000 + (Get-MissesFor 512.0)) (1048576 + $BYTES_TRUE) $true }
        # Ordering proofs: the r2 value is a CORRECT 4096 in every one of these,
        # but an earlier gate must win. If any of them reports D01-CONFIRMED, the
        # gate chain is not actually ordered and a broken run could look green.
        'ORDER_COLD'         { $script:bm['driverCold'] = 'false' }
        'ORDER_N2'           { $script:jAfter = New-Sample (1000 + (Get-MissesFor 4096.0)) (1048576 + [int64]($BYTES_TRUE * 1.05)) $true }
        'ORDER_L2'           { $script:l2Before = [int64]2 }
        'ORDER_H'            { $script:bm['secondHitRate'] = '-1.0000' }
        # Adversarial: 4096 r2 but with a zero counter -> division impossible.
        'ZERO_DMISSES'       { $script:jAfter = New-Sample ([int64]1000) (1048576 + $BYTES_TRUE) $true }
        default { Write-Host "SELFTEST unknown case '$name'"; exit 90 }
    }
}

if (-not $Case) { Write-Host 'SELFTEST: -Case required'; exit 90 }
Set-Case $Case

# Any error inside the judged block must be loud, never swallowed: a silent
# error would leave $state/$rc at their defaults and look like a real verdict.
$ErrorActionPreference = 'Stop'
$global:ErrorActionPreference = 'Stop'
$script:errs = 0
$probe = $block -replace '(?m)^\s*exit \$rc\s*$', 'Dump-Lines; Write-Output ("SELFTEST_STATE=" + $state); Write-Output ("SELFTEST_RC=" + $rc); exit $rc'
if ($probe -eq $block) { Write-Host 'SELFTEST FAIL: could not locate the trailing exit $rc'; exit 90 }
try {
    Invoke-Expression $probe
} catch {
    Write-Host "SELFTEST FAIL (exception in judged block): $($_.Exception.Message)"
    exit 92
}
# Not reached; the block always exits.
Write-Host 'SELFTEST FAIL: block returned without exit'
exit 91