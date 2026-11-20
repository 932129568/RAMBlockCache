# test-p4-ratio.ps1  [JUDGE SELFTEST - keep: this is the proof that the criteria have teeth]
# WHAT IT PROVES: the shipped Get-P4RatioState: two-sided bands, exact 0.5/2.0 fail, and the M1/M2/M3 mutations that prove the judge can bite
# Extracts Get-P4RatioState + Format-P4RatioNote from the SHIPPED
# RB-write-load.ps1 and drives them through mutation cases, so the judge is
# proven against the real code rather than against a copy of it.
#
# WHAT MUST BE PROVEN (the failure this suite exists for):
#   the previous judge was `ratio >= 0.9 -> PASS`, one-sided. The first real run
#   measured 3,655.68 MB / 1,800 MB = 2.03 and it PASSED. A judge that passes a
#   100% overshoot has no teeth.
#
#   M1  denominator = direct writes only (1,800 MB), dWrite = 3,655.68 MB
#       -> ratio 2.03  => must be FAIL with the new judge (old judge: PASS)
#   M2  denominator = 3,600 MB, dWrite cut to 1,800 MB
#       -> ratio 0.50  => must be FAIL
#   M3  real run: 3,600 MB denominator, 3,655.68 MB observed
#       -> ratio 1.015 => must be PASS
#   plus the band edges and the MISSING / zero cases.
#
# exit 0 = every case matched; 1 = at least one mismatch.

param(
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) 'RB-write-load.ps1')
)

$ErrorActionPreference = 'Stop'
$text = [IO.File]::ReadAllText($Source)
$s = $text.IndexOf('function Get-P4RatioState')
$e = $text.IndexOf('function Write-RealFile')
if ($s -lt 0 -or $e -lt $s) { Write-Host 'SELFTEST FAIL: cannot locate P4 judge functions'; exit 90 }
Invoke-Expression $text.Substring($s, $e - $s)

$fail = 0
function T([string]$what, [string]$got, [string]$want) {
    if ($got -eq $want) { Write-Output ("  ok    {0,-46} -> {1}" -f $what, $got) }
    else { Write-Output ("  FAIL  {0,-46} -> got {1}, want {2}" -f $what, $got, $want); $script:fail++ }
}

# The three numbers that matter, straight from the real run's log.
$MB = 1MB
$obsReal     = 3655.68 * $MB     # dWrite observed 2026-11-20 15:18:40
$denDirect   = 1800.0 * $MB      # direct writes only (the old, wrong denominator)
$denFull     = 3600.0 * $MB      # direct + copy writes (the correct denominator)
$obsHalf     = 1800.0 * $MB

Write-Output "=== M1: old denominator (direct writes only) with the real observed dWrite ==="
$r = Get-P4RatioState $obsReal $denDirect
Write-Output ("  ratio = {0:P2}" -f ($obsReal / $denDirect))
T 'M1 ratio 2.03 -> FAIL (old judge said PASS)' $r 'FAIL'

Write-Output "=== M2: correct denominator, but only half the bytes crossed ==="
Write-Output ("  ratio = {0:P2}" -f ($obsHalf / $denFull))
T 'M2 ratio 0.50 -> FAIL' (Get-P4RatioState $obsHalf $denFull) 'FAIL'

Write-Output "=== M3: the real run (correct denominator) ==="
Write-Output ("  ratio = {0:P4}" -f ($obsReal / $denFull))
T 'M3 ratio 1.0155 -> PASS' (Get-P4RatioState $obsReal $denFull) 'PASS'

Write-Output "=== M4: round 1 and round 2 as measured ==="
$r1 = 1863.68 * $MB; $r2 = 1792.00 * $MB; $denRound = 1800.0 * $MB
Write-Output ("  R1 ratio = {0:P2}   R2 ratio = {1:P2}" -f ($r1/$denRound), ($r2/$denRound))
T 'R1 1863.68/1800 = 103.5% -> PASS' (Get-P4RatioState $r1 $denRound) 'PASS'
T 'R2 1792.00/1800 = 99.6%  -> PASS' (Get-P4RatioState $r2 $denRound) 'PASS'

Write-Output "=== band edges (inclusive where documented) ==="
T 'ratio 0.00 (dWrite 0)     -> FAIL' (Get-P4RatioState 0.0 $denFull) 'FAIL'
T 'ratio 0.4999             -> FAIL' (Get-P4RatioState (0.4999*$denFull) $denFull) 'FAIL'
T 'ratio 0.5000             -> FAIL (exactly half = a whole source missing)' (Get-P4RatioState (0.5000*$denFull) $denFull) 'FAIL'
T 'ratio 0.5001             -> WARN-LOW' (Get-P4RatioState (0.5001*$denFull) $denFull) 'WARN-LOW'
T 'ratio 0.8999             -> WARN-LOW' (Get-P4RatioState (0.8999*$denFull) $denFull) 'WARN-LOW'
T 'ratio 0.9000             -> PASS' (Get-P4RatioState (0.9000*$denFull) $denFull) 'PASS'
T 'ratio 1.0000             -> PASS' (Get-P4RatioState (1.0000*$denFull) $denFull) 'PASS'
T 'ratio 1.1500             -> PASS' (Get-P4RatioState (1.1500*$denFull) $denFull) 'PASS'
T 'ratio 1.1501             -> WARN-HIGH' (Get-P4RatioState (1.1501*$denFull) $denFull) 'WARN-HIGH'
T 'ratio 1.9999             -> WARN-HIGH' (Get-P4RatioState (1.9999*$denFull) $denFull) 'WARN-HIGH'
T 'ratio 2.0000             -> FAIL (exactly double = a double count)' (Get-P4RatioState (2.0000*$denFull) $denFull) 'FAIL'
T 'ratio 2.0300 (the real M1) -> FAIL' (Get-P4RatioState (2.0300*$denFull) $denFull) 'FAIL'

Write-Output "=== MISSING discipline: unreadable is never 0 ==="
T 'dWrite null              -> MISSING' (Get-P4RatioState $null $denFull) 'MISSING'
T 'derived 0 bytes is MISSING, not FAIL' (Get-P4RatioState $null 0.0) 'MISSING'

Write-Output "=== note text carries the ratio AND the band, so a reader can recheck ==="
$n1 = Format-P4RatioNote $obsReal $denDirect 'FAIL'
$n3 = Format-P4RatioNote $obsReal $denFull 'PASS'
Write-Output ("  M1 note : $n1")
Write-Output ("  M3 note : $n3")
T 'M1 note quotes ratio 203%' ($n1 -match '203') 'True'
T 'M1 note names the outside-band reason' ($n1 -match 'outside \(0\.5, 2\.0\)') 'True'
T 'M3 note names the inside band' ($n3 -match 'inside \[0.90, 1.15\]') 'True'

Write-Output ""
if ($fail -eq 0) { Write-Output 'ALL P4 RATIO TESTS OK' } else { Write-Output "$fail FAILED" }
exit $(if ($fail -eq 0) { 0 } else { 1 })
