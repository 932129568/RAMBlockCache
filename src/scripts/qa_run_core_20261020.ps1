$ErrorActionPreference = "Continue"
# This session sets both HTTPS_PROXY and https_proxy; Start-Process's env block
# rejects case-duplicate keys. Drop the lowercase variants (proxies not needed here).
foreach ($n in @("https_proxy","http_proxy","all_proxy","no_proxy")) {
    if (Test-Path "Env:$n") { Remove-Item "Env:$n" }
}
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out  = Join-Path $root "src\core\build\qa-20261020"
$tests = @("test_auditfix","test_bigread","test_cache","test_counter","test_counter_seed",
           "test_evict_wire","test_flush_many","test_geom","test_mixed","test_parflush",
           "test_regress","test_release","test_v5","test_v6",
           "edge_v3","edge_v4","stress_v3","stress_v4","test_write_ex","test_wsconc","offchk")
$pass = 0; $fail = 0
# [FIX QA5-10] per-test timeout: a hung test no longer hangs the QA session
# forever (Start-Process -Wait has no timeout).
$TimeoutMs = 120000
foreach ($t in $tests) {
    $exe = "$out\$t.exe"
    if (-not (Test-Path -LiteralPath $exe)) { Write-Output "$t`: MISSING BINARY"; $fail++; continue }
    $log = "$out\run_$t.txt"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -WorkingDirectory $out -NoNewWindow -PassThru `
         -RedirectStandardOutput $log -RedirectStandardError "$out\err_$t.txt"
    $timedOut = -not $p.WaitForExit($TimeoutMs)
    $sw.Stop()
    if ($timedOut) {
        try { $p.Kill() } catch { }
        Write-Output ("{0,-18} TIMEOUT after {1:N0}ms (killed)" -f $t, $sw.ElapsedMilliseconds)
        $fail++
        continue
    }
    $rc = $p.ExitCode
    $tail = Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Last 4
    $verdict = if ($rc -eq 0) { "PASS"; $pass++ } else { "FAIL"; $fail++ }
    Write-Output ("{0,-18} rc={1,-4} {2,-5} {3,6:N0}ms | {4}" -f $t, $rc, $verdict, $sw.ElapsedMilliseconds, ($tail -join " / "))
}
Write-Output "=== RUN DONE: pass=$pass fail=$fail ==="
# [FIX QA5-10] real exit code for CI.
if ($fail -gt 0) { exit 1 }
exit 0
