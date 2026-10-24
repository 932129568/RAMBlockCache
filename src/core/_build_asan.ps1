# _build_asan.ps1 -- ASan build & run for all core tests (audit-fix round)
# ASCII only.
$ErrorActionPreference = "Continue"
$vc = "C:\Program1\VC\Tools\MSVC\14.44.35207"
$sdk = "C:\Program Files (x86)\Windows Kits\10"
$sdkv = "10.0.26100.0"
$env:INCLUDE = "$vc\include;$sdk\Include\$sdkv\ucrt;$sdk\Include\$sdkv\um;$sdk\Include\$sdkv\shared"
$env:LIB     = "$vc\lib\x64;$sdk\Lib\$sdkv\ucrt\x64;$sdk\Lib\$sdkv\um\x64"
$env:PATH    = "$vc\bin\Hostx64\x64;$env:SystemRoot\System32;$env:SystemRoot;$env:PATH"
$core = $PSScriptRoot
Set-Location $core
if (-not (Test-Path build)) { New-Item -ItemType Directory build | Out-Null }

# [FIX #9 2026-10-21] 列表与 build_all.bat 对齐（原缺 test_write_ex/test_wsconc，
# 与 .bat 权威清单漂移；.bat 在沙箱禁 cmd 通道时不可跑，本脚本是其等价入口）。
$Tests = @(
  "test_cache","test_counter","test_geom","test_mixed","test_parflush","test_regress",
  "test_release","test_v5","test_v6","test_bigread","test_flush_many",
  "edge_v3","edge_v4","stress_v3","stress_v4","bench_readhit","test_auditfix",
  "test_write_ex","test_wsconc"
)
$ok=0; $fail=0; $runFail=0
foreach ($t in $Tests) {
  $exe = Join-Path $core "build\asan_$t.exe"
  & cl /nologo /Od /Zi /W4 /utf-8 /I. /fsanitize=address (Join-Path $core "$t.c") cache.c /Fe:$exe /Fo:build\ > "build\asan_$t.rc.log" 2>&1
  if (Test-Path $exe) { $ok++ } else { $fail++; Write-Output "[FAIL-asan] $t"; Get-Content "build\asan_$t.rc.log" | Select-Object -First 6 }
}
$exe = Join-Path $core "build\asan_test_evict_wire.exe"
& cl /nologo /Od /Zi /W4 /utf-8 /I. /fsanitize=address /DRB_EVICT_HOOK_EXTERNAL (Join-Path $core "test_evict_wire.c") cache.c /Fe:$exe /Fo:build\ > "build\asan_test_evict_wire.rc.log" 2>&1
if (Test-Path $exe) { $ok++ } else { $fail++; Write-Output "[FAIL-asan] test_evict_wire" }

Write-Output "ASAN_OK=$ok ASAN_FAIL=$fail"
foreach ($t in ($Tests + @("test_evict_wire"))) {
  $exe = Join-Path $core "build\asan_$t.exe"
  if (Test-Path $exe) {
    # [FIX R9-15] 输出名与 run_all.bat 逐字对齐（原 asanrun_ 前缀导致 check_repo
    #  freshness 门禁按 asan_<t>.out 配对时永远拿到 run_all.bat 的陈旧证据）。
    & $exe > "build\asan_$t.out" 2> "build\asan_$t.err"
    if ($LASTEXITCODE -ne 0) { Write-Output "$t exit=$LASTEXITCODE"; $runFail++ }
  }
}
Write-Output "ASAN_RUN_FAIL=$runFail"
Write-Output "ASAN_SUMMARY ok=$ok fail=$fail run_fail=$runFail"
