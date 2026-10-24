# [FIX QA5-7] repeat/concurrency harness — fail-closed rewrite
# Original defects (2026-10-21 QA round): missing binaries reported fails=0 (fake
# green, $p was $null and its ExitCode access was swallowed), fixed TEMP names
# (concurrent instances overwrote each other's evidence), no timeout (a hung test
# hung the QA session forever), always exit 0.
$ErrorActionPreference = "Stop"
foreach ($n in @("https_proxy","http_proxy")) { if (Test-Path "Env:$n") { Remove-Item "Env:$n" } }
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out  = Join-Path $root "src\core\build"
# [FIX QA5-5] cover the new test wave too (write_ex / wsconc / auditfix).
$tests = @("test_parflush","test_flush_many","stress_v3","stress_v4","test_write_ex","test_wsconc","test_auditfix")
$fails = 0
foreach ($t in $tests) {
    $exe = Join-Path $out "$t.exe"
    # [FIX QA5-7a] missing/corrupt binary = failure, never fake green.
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Output "$t`: MISSING binary $exe"
        $fails++
        continue
    }
    for ($i=1; $i -le 10; $i++) {
        # [FIX QA5-7b] instance-unique temp files (PID + test + iter).
        $tmpO = Join-Path $env:TEMP "qa_rep_${PID}_${t}_$i.txt"
        $tmpE = Join-Path $env:TEMP "qa_repe_${PID}_${t}_$i.txt"
        try {
            $p = Start-Process -FilePath $exe -WorkingDirectory $out -NoNewWindow -Wait -PassThru `
                 -RedirectStandardOutput $tmpO -RedirectStandardError $tmpE
        } catch {
            Write-Output "$t iter $i`: START-FAIL $($_.Exception.Message)"
            $fails++
            continue
        }
        if ($p.ExitCode -ne 0) {
            Write-Output "$t iter $i`: FAIL rc=$($p.ExitCode)"
            if (Test-Path $tmpO) { Get-Content $tmpO | Select-Object -Last 6 }
            $fails++
        }
        Remove-Item $tmpO,$tmpE -ErrorAction SilentlyContinue
    }
    Write-Output "$t`: 10 iterations done"
}
Write-Output "=== repeat-10 done: fails=$fails ==="
# [FIX QA5-7c] real exit code: any failure -> non-zero.
if ($fails -gt 0) { exit 1 }
exit 0
