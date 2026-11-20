# test-cmd-rc-table.ps1  [JUDGE SELFTEST - keep: this is the proof that the criteria have teeth]
# WHAT IT PROVES: RB-write-load.cmd: every rc the wrapper can emit maps to the documented VERDICT row, and the real exit code propagates
# Proves the RB-write-load.cmd wrapper's rc -> VERDICT mapping for EVERY code the
# .ps1 can actually emit (0/1/3/4/5) AND for the codes it cannot (2/6/7/9), by
# running the REAL wrapper against a stub .ps1 that exits with a chosen code.
#
# Why this is separate from test-cmd-rc-table-bench.ps1: that one exercises the BENCH
# wrapper (RB-bench-miss-coalescing.cmd), which already has the undocumented-rc
# fallback rows. This one exercises the WRITE-LOAD wrapper, which does not. The
# open question for team-lead was exactly "does rc=2 or rc=7 print an empty
# VERDICT row?" -- an unproven claim until this file runs.
#
# Nothing here reads C:, D: or the driver: the stub replaces the script, and the
# wrapper is the only thing under test.

# PATH: this test lives in deliverables/tests/ and exercises deliverables/RB-write-load.cmd.
# Derived from $PSScriptRoot so the test keeps working if the repo moves; a
# hardcoded absolute path silently breaks the moment it does.
param(
    [string]$Repo = (Split-Path -Parent $PSScriptRoot),
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'

# String keys on purpose -- see the note in test-cmd-rc-table-bench.ps1: integer keys
# are positional indices into an OrderedDictionary, so $cases[6] on a short
# table returns $null, an empty expectation matches every printed row, and the
# case reports PASS while comparing nothing.
#
# Expected values are the text that follows "VERDICT   : " in the wrapper.
# '<EMPTY>' means "the wrapper prints NO verdict row at all" -- that is the
# finding under test, not an accident.
$cases = [ordered]@{
    '0' = 'PASS'
    '1' = 'UNKNOWN'
    '2' = '<EMPTY>'
    '3' = 'FAIL'
    '4' = 'RUNABORTED'
    '5' = 'INCONCLUSIVE'
    '6' = '<EMPTY>'
    '7' = '<EMPTY>'
    '9' = '<EMPTY>'
}

$fail = 0
$work = Join-Path $env:TEMP ('rb-rwl-stub-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work -Force | Out-Null

# ---- DESTRUCTIVE-SCOPE GUARD (added after this file destroyed the 15:18 log) --
# The first version of this file cleaned the wrapper's log glob BEFORE running,
# i.e. `Remove-Item 'RB-write-load_*.log'`. That glob also matched the one
# frozen, unrepeatable artifact of the single real machine run
# (RB-write-load_20261120_151840.log) and deleted it. The run it belonged to can
# never be reproduced: the user had exactly one elevated execution and it is
# spent. A test harness must never be able to delete evidence it did not create.
#
# So: nothing here deletes anything matching the product's own log name. The
# stub writes a log prefixed rb-rwl-stub-* instead, and the wrapper's "newest
# log" discovery is exercised against a PRIVATE %TEMP% subfolder that is the
# only thing ever removed.
#
# ATTRIBUTION -- what can and cannot be established (2026-11-20):
#   1. This file is the only place in the repo that ever globbed the product's log
#      name. `Remove-Item -Filter 'RB-write-load_*.log'` matches every file of
#      that name present at that moment, so a SINGLE run deleting all 13 of them is
#      mechanically possible -- it does not require one run per file.
#   2. The three files that survived are lowercase names of a DIFFERENT shape
#      (rb-write-load-manual.log, -run1.log, -run2.log). A whole-directory sweep
#      (Storage Sense, a "clean temp" tool, manual Ctrl+A delete) would have taken
#      those too. They survived, so what happened was a delete of the
#      RB-write-load_<timestamp>.log PATTERN, not a directory cleanup.
#   CONCLUSION: "consistent with the harness glob; cannot be independently
#   confirmed because the logs are gone". There is no audit trail, so the 13
#   individual deletions cannot be attributed file-by-file. Do NOT upgrade this to
#   "confirmed" in any report.
$sandbox = Join-Path $env:TEMP ('rb-rwl-sandbox-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $sandbox -Force | Out-Null
$env:TEMP_BACKUP = $env:TEMP
$env:TEMP = $sandbox          # the wrapper globs %TEMP%; confine it to the sandbox

Copy-Item -LiteralPath (Join-Path $Repo 'RB-write-load.cmd') -Destination $work -Force

# The stub accepts the wrapper's real argument shapes and writes a durable log
# named the way the wrapper globs for, so the reporting path is exercised too.
$stub = @'
param([switch]$DryRun, [int]$Rounds = 2, [int]$SizeMB = 900, [string]$TargetDir = '')
$rc = [int]$env:STUB_RC
$log = Join-Path $env:TEMP ('RB-write-load_stub_{0}.log' -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
Add-Content -LiteralPath $log -Value ("stub: DryRun=$DryRun Rounds=$Rounds SizeMB=$SizeMB TargetDir=$TargetDir rc=$rc")
Add-Content -LiteralPath $log -Value ("TERMINAL rc={0} crash=False P1=True P2=True P3=True P4=PASS P5=True P0=ZERO-UNINFORMATIVE dMisses=0 metaFlush=2 wall=3.0min" -f $rc)
if ($rc -ne 0) { [Console]::Error.WriteLine("stub stderr: synthetic failure path, rc=$rc") }
exit $rc
'@
Set-Content -LiteralPath (Join-Path $work 'RB-write-load.ps1') -Value $stub -Encoding UTF8

foreach ($rcKey in $cases.Keys) {
    $rc   = [int]$rcKey
    $want = $cases[$rcKey]
    if (-not $want) { "---- stub rc=$rc ----  FAIL: empty expectation (harness bug)"; $fail++; continue }
    $outF = Join-Path $work ("out-$rc.txt")
    $errF = Join-Path $work ("err-$rc.txt")
    $env:STUB_RC = [string]$rc
    $env:RB_NO_PAUSE = '1'
    $p = Start-Process -FilePath $env:ComSpec -ArgumentList '/c', 'RB-write-load.cmd' `
        -WorkingDirectory $work -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $outF -RedirectStandardError $errF
    $real = $p.ExitCode
    $out  = if (Test-Path $outF) { Get-Content -LiteralPath $outF -Raw } else { '' }
    $err  = if (Test-Path $errF) { Get-Content -LiteralPath $errF -Raw } else { '' }

    $exitLine    = (($out -split "\r?\n") | Where-Object { $_ -match '^EXIT CODE :' }  | Select-Object -First 1)
    $verdictLine = (($out -split "\r?\n") | Where-Object { $_ -match '^VERDICT   :' }  | Select-Object -First 1)

    $okRc  = ($real -eq $rc)
    $okEx  = ($exitLine -match ("EXIT CODE : {0}\b" -f $rc))
    if ($want -eq '<EMPTY>') {
        $okVer = (-not $verdictLine)          # we EXPECT no row; a row is the bug
    } else {
        $okVer = ($verdictLine -like ("VERDICT   : {0}*" -f $want))
    }

    "---- stub rc=$rc ----"
    "  want verdict : $want"
    "  printed      : $(if ($verdictLine) { $verdictLine.Trim() } else { '<NONE / EMPTY ROW>' })"
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
