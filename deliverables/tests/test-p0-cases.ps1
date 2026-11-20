# test-p0-cases.ps1  [JUDGE SELFTEST - keep: this is the proof that the criteria have teeth]
# WHAT IT PROVES: drives test-p0-control.ps1 across all 13 cases and checks the state name each one reaches, in a CHILD process so exit codes are real
# Drives test-p0-control.ps1 across every case, compares the emitted
# SELFTEST_STATE against the expected state, and prints a per-case line.
# This is the "prove the judge has teeth" step: a state name that no case can
# reach is a state that was never verified.
#
# Each case is run in a CHILD pwsh process. Calling the harness with '&' from
# here would share this runspace, where 'exit' inside it does not behave like a
# process exit code -- the first version of this driver got $LASTEXITCODE = $null
# for every case and reported 11 spurious FAILs while every state was correct.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$st   = Join-Path $here 'test-p0-control.ps1'
$pw   = if ($PSVersionTable.PSEdition -eq 'Core') { (Get-Process -Id $PID).Path } else { 'powershell.exe' }

# case -> expected state (independent of what the harness asserts internally)
$cases = [ordered]@{
    'ZERO'      = 'ZERO-UNINFORMATIVE'   # dMisses=0, dReadReq=0 -> premise not exercised
    'ZEROREAD'  = 'ZERO-AS-EXPECTED'     # dMisses=0, dReadReq>0 -> genuinely quiet
    'ZERONOREQ' = 'ZERO-UNINFORMATIVE'   # dMisses=0, ReadReq unreadable -> not informative
    'LOW1'      = 'LOW-AS-EXPECTED'
    'ANCHOR'    = 'LOW-AS-EXPECTED'
    'ANCHORP1'  = 'LOW-AS-EXPECTED'
    'GATE'      = 'LOW-AS-EXPECTED'
    'GATEP1'    = 'HIGH-UNEXPECTED'
    'BIGLEAK'   = 'HIGH-UNEXPECTED'
    'UNREAD'    = 'UNREADABLE'
    'L2ON'      = 'UNREADABLE'
    'N2BAD'     = 'LOW-AS-EXPECTED'
    'N2ZERO'    = 'LOW-AS-EXPECTED'
}

$pass = 0; $fail = 0; $bad = @()
foreach ($c in $cases.Keys) {
    $want = $cases[$c]
    $out  = & $pw -NoProfile -ExecutionPolicy Bypass -File $st -Case $c -Expect $want 2>&1 | Out-String
    $rc   = $LASTEXITCODE
    $line = ($out -split "\r?\n" | Where-Object { $_ -match '^SELFTEST_STATE=' } | Select-Object -First 1)
    $got  = if ($line) { ($line -replace '^SELFTEST_STATE=', '').Trim() } else { '<none>' }
    if ($rc -eq 0 -and $got -eq $want) {
        $pass++
        "PASS  {0} state={1}  (rc=0)" -f $c.PadRight(9), $got
    } else {
        $fail++; $bad += $c
        "FAIL  {0} want={1} got={2} rc={3}" -f $c.PadRight(9), $want, $got, $rc
        ($out -split "\r?\n" | Where-Object { $_ } | Select-Object -Last 8) | ForEach-Object { "        | $_" }
    }
}
""
"SUMMARY: pass=$pass fail=$fail"
if ($bad.Count) { "FAILED CASES: $($bad -join ', ')"; exit 1 }
exit 0
