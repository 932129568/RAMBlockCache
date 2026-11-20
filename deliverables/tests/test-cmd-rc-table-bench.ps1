# test-cmd-rc-table-bench.ps1  [JUDGE SELFTEST - keep: this is the proof that the criteria have teeth]
# WHAT IT PROVES: RB-bench-miss-coalescing.cmd: same rc -> VERDICT contract, including the undocumented-code fallback rows
# Proves the .cmd wrapper's rc -> VERDICT mapping for EVERY documented code,
# including rc=6, by running the REAL .cmd against a stub .ps1 that exits with a
# chosen code. Nothing here reads C:, D: or the driver: the stub replaces the
# script, and the wrapper is the only thing under test.
#
# Why this exists: a verdict row that no run can produce is a verdict row that
# was never proven to have teeth. rc=6 was added this round (D01-INCONCLUSIVE:
# "usable data, in neither band") and its whole reason for existing is that such
# a physical state must NOT be reported as INVALID -- which only means something
# if the wrapper actually prints it.
#
# Contract: exit 0 = every case matched its expected verdict AND the wrapper
# propagated the real exit code; 1 = at least one mismatch.

# PATH: this test lives in deliverables/tests/ and exercises deliverables/RB-bench-miss-coalescing.cmd.
param(
    [string]$Repo = (Split-Path -Parent $PSScriptRoot),
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'

# Keys are STRINGS on purpose. With integer keys, OrderedDictionary's indexer
# treats the argument as a POSITIONAL index, so $cases[9] on a 9-element table
# returns $null instead of the rc=9 entry -- and the 'VERDICT   : *' pattern
# that an empty expectation produced then matched the printed row by accident.
# That is the "empty set passes" failure this whole suite exists to catch,
# found inside the harness itself: rc=9 was reported PASS while the assertion
# compared nothing. Fixed by string keys + a non-empty expectation check.
$cases = [ordered]@{
    '0' = 'D01-CONFIRMED'
    '1' = 'UNKNOWN'
    '2' = 'UNDOCUMENTED rc=2'
    '3' = 'D01-REFUTED or INVALID'
    '4' = 'RUNABORTED'
    '5' = 'MISSING'
    '6' = 'D01-INCONCLUSIVE'
    '7' = 'UNDOCUMENTED rc=7'
    '9' = 'UNDOCUMENTED rc=9'
}

$fail = 0
$work = Join-Path $env:TEMP ('rb-cmd-stub-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work -Force | Out-Null

# ---- DESTRUCTIVE-SCOPE GUARD -------------------------------------------------
# This file used to clean the wrapper's log glob before running:
#     Get-ChildItem $env:TEMP -Filter 'RB-bench-miss_*.log' | Remove-Item
# to make "newest log" deterministic. The same pattern, copied into the sibling
# test-cmd-rc-table.ps1 (then named _cmd-rc-stub-test-rwl.ps1), DELETED the single frozen real-run log
# (RB-write-load_20261120_151840.log) -- an unrepeatable artifact, since the
# user's one elevated run is already spent. That one is gone. This one got lucky:
# no real bench run has produced a log yet.
#
# A harness must never be able to delete evidence it did not create. So:
# redirect $env:TEMP into a private sandbox for the duration of the run, write
# stub logs under a stub-only prefix, and remove ONLY those two absolute paths.
$sandbox = Join-Path $env:TEMP ('rb-bench-sandbox-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $sandbox -Force | Out-Null
$env:TEMP_BACKUP = $env:TEMP
$env:TEMP = $sandbox          # the wrapper globs %TEMP%; confine it to the sandbox

Copy-Item -LiteralPath (Join-Path $Repo 'RB-bench-miss-coalescing.cmd') -Destination $work -Force

# The stub accepts the wrapper's real argument shapes and writes a durable log
# with the marker the wrapper greps for, so the reporting path is exercised too.
$stub = @'
param([switch]$DryRun, [int]$Passes = 2)
$rc = [int]$env:STUB_RC
$log = Join-Path $env:TEMP ('RB-bench-miss_stub_{0}.log' -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
Add-Content -LiteralPath $log -Value ("stub: DryRun=$DryRun Passes=$Passes rc=$rc")
Add-Content -LiteralPath $log -Value ("TERMINAL rc={0} state=STUB dMisses=123 dBytesMiss=456 B=789 S=1 h=0.84 m1=2 m2=3 denom=4 r2=5 N2dev=0 passes=2" -f $rc)
if ($rc -ne 0) { [Console]::Error.WriteLine("stub stderr: synthetic failure path, rc=$rc") }
exit $rc
'@
Set-Content -LiteralPath (Join-Path $work 'RB-bench-miss-coalescing.ps1') -Value $stub -Encoding UTF8

foreach ($rcKey in $cases.Keys) {
    $rc   = [int]$rcKey
    $want = $cases[$rcKey]
    if (-not $want) { "---- stub rc=$rc ----  FAIL: empty expectation (harness bug)"; $fail++; continue }
    $outF = Join-Path $work ("out-$rc.txt")
    $errF = Join-Path $work ("err-$rc.txt")
    $env:STUB_RC = [string]$rc
    $env:RB_NO_PAUSE = '1'
    $p = Start-Process -FilePath $env:ComSpec -ArgumentList '/c', 'RB-bench-miss-coalescing.cmd' `
        -WorkingDirectory $work -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $outF -RedirectStandardError $errF
    $real = $p.ExitCode
    $out  = if (Test-Path $outF) { Get-Content -LiteralPath $outF -Raw } else { '' }
    $err  = if (Test-Path $errF) { Get-Content -LiteralPath $errF -Raw } else { '' }

    $exitLine    = (($out -split "\r?\n") | Where-Object { $_ -match '^EXIT CODE :' }  | Select-Object -First 1)
    $verdictLine = (($out -split "\r?\n") | Where-Object { $_ -match '^VERDICT   :' }  | Select-Object -First 1)

    $okRc  = ($real -eq $rc)
    $okEx  = ($exitLine -match ("EXIT CODE : {0}\b" -f $rc))
    $okVer = ($verdictLine -like ("VERDICT   : {0}*" -f $want))

    "---- stub rc=$rc ----"
    "  want verdict : $want"
    "  printed      : $(if ($verdictLine) { $verdictLine.Trim() } else { '<NONE>' })"
    "  exit line    : $(if ($exitLine) { $exitLine.Trim() } else { '<NONE>' })"
    "  real exit    : $real"
    "  stderr       : $(if ($err -and $err.Trim()) { $err.Trim() } else { '<empty>' })"
    if ($okRc -and $okEx -and $okVer) { "  RESULT       : PASS" }
    else {
        "  RESULT       : FAIL  (rc-ok=$okRc exitline-ok=$okEx verdict-ok=$okVer)"
        $fail++
    }
}

if (-not $Keep) {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
} else {
    "workdir kept: $work"
}
# Remove ONLY the sandbox this file created (guard: never a bare glob on %TEMP%).
if ($sandbox -and (Test-Path -LiteralPath $sandbox)) {
    Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue
}
if ($env:TEMP_BACKUP) { $env:TEMP = $env:TEMP_BACKUP }
Remove-Item Env:TEMP_BACKUP -ErrorAction SilentlyContinue
Remove-Item Env:STUB_RC -ErrorAction SilentlyContinue
Remove-Item Env:RB_NO_PAUSE -ErrorAction SilentlyContinue

""
"SUMMARY: fails=$fail across $($cases.Count) cases"
exit $(if ($fail -eq 0) { 0 } else { 1 })
