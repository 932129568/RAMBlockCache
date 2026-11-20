# test-rmc-cases.ps1  [JUDGE SELFTEST - keep: this is the proof that the criteria have teeth]
# WHAT IT PROVES: drives test-rmc-selftest.ps1 across every branch of the shipped
# bench verdict block and compares the real process exit code to the expected one.
# Runs every branch of the shipped verdict block through test-rmc-selftest.ps1 and
# compares the real process exit code to the expected one.

param([string]$Pwsh = 'C:\Program Files\PowerShell\7\pwsh.exe')

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$self = Join-Path $here 'test-rmc-selftest.ps1'

# case name -> @(expected rc, expected state substring)
$cases = [ordered]@{
    'CONFIRMED'       = @(0, 'D01-CONFIRMED')
    'REFUTED'         = @(3, 'D01-REFUTED')
    'INCONCLUSIVE'    = @(6, 'D01-INCONCLUSIVE')
    'N2_FAIL'         = @(3, 'INVALID')
    'COLD_FALSE'      = @(3, 'INVALID')
    'COLD_MISSING'    = @(3, 'INVALID')
    'HITRATE_NEG'     = @(3, 'INVALID')
    'HITRATE_MISSING' = @(3, 'INVALID')
    'BYTES_MISMATCH'  = @(3, 'INVALID')
    'BYTES_MISSING'   = @(3, 'INVALID')
    'L2_ON'           = @(3, 'INVALID')
    'L2_MISSING'      = @(5, 'MISSING')
    'COUNTER_MISSING' = @(5, 'MISSING')
    'BENCH_RC'        = @(3, 'INVALID')
    'PASSES_3'        = @(3, 'INVALID')
    'DENOM_LE_ZERO'   = @(3, 'INVALID')
    'R2_OVER_CEILING' = @(3, 'INVALID')
    'N2_UNTESTABLE'   = @(5, 'MISSING')
    # band edges, one unit either side of every boundary
    'EDGE_3767'       = @(0, 'D01-CONFIRMED-MORE-FRAGMENTED')
    'EDGE_3769'       = @(0, 'D01-CONFIRMED')
    'EDGE_4423'       = @(0, 'D01-CONFIRMED')
    'EDGE_4425'       = @(6, 'D01-INCONCLUSIVE')
    'EDGE_117964'     = @(6, 'D01-INCONCLUSIVE')
    'EDGE_117966'     = @(3, 'D01-REFUTED')
    'EDGE_144178'     = @(3, 'D01-REFUTED')
    'EDGE_144180'     = @(3, 'INVALID')
    'R2_512'          = @(0, 'D01-CONFIRMED-MORE-FRAGMENTED')
    # ordering proofs: a correct r2=4096 must NOT win against a failed gate
    'ORDER_COLD'      = @(3, 'INVALID')
    'ORDER_N2'        = @(3, 'INVALID')
    'ORDER_L2'        = @(3, 'INVALID')
    'ORDER_H'         = @(3, 'INVALID')
    'ZERO_DMISSES'    = @(3, 'INVALID')
}

$bad = 0
"{0,-18} {1,-6} {2,-6} {3,-20} {4}" -f 'CASE', 'WANT', 'GOT', 'STATE', 'RESULT'
"-" * 62
foreach ($k in $cases.Keys) {
    $out = & $Pwsh -NoProfile -ExecutionPolicy Bypass -File $self -Case $k 2>&1
    $got = $LASTEXITCODE
    $want = $cases[$k][0]
    $wantState = $cases[$k][1]
    $st = ($out | Where-Object { "$_" -match '^SELFTEST_STATE=' }) -replace '^SELFTEST_STATE=', ''
    $ok = ($got -eq $want) -and ($st -eq $wantState)
    if (-not $ok) { $bad++ }
    "{0,-18} {1,-6} {2,-6} {3,-20} {4}" -f $k, $want, $got, $(if ($st) { $st } else { '(none)' }), $(if ($ok) { 'ok' } else { '*** MISMATCH ***' })
    # Any stderr noise means the harness or the block errored silently in a way
    # the rc comparison cannot see. Count it as a failure, not as a warning.
    $err = $out | Where-Object { "$_" -match 'SELFTEST FAIL|不会被识别|Exception|无法|not recognized' }
    if ($err) { $bad++; $out | ForEach-Object { "      ! $_" } }
}
"-" * 62
if ($bad -eq 0) { "ALL $($cases.Count) CASES OK (rc AND state matched)" } else { "$bad of $($cases.Count) FAILED" }
exit $(if ($bad -eq 0) { 0 } else { 1 })