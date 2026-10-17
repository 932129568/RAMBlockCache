# _build_all.ps1 -- user-mode core test build & run (Release only, quick gate for audit-fix round)
# ASCII only (PS 5.1 + BOM-less UTF-8 pitfall).
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

$Tests = @(
  "test_cache","test_counter","test_geom","test_mixed","test_parflush","test_regress",
  "test_release","test_v5","test_v6","test_bigread","test_flush_many",
  "edge_v3","edge_v4","stress_v3","stress_v4","bench_readhit","test_auditfix"
)
$relOk=0; $relFail=0; $runFail=0
foreach ($t in $Tests) {
  $src = Join-Path $core "$t.c"
  $exe = Join-Path $core "build\$t.exe"
  & cl /nologo /O2 /W4 /utf-8 /I. $src cache.c /Fe:$exe /Fo:build\ > "build\$t.rc.log" 2>&1
  if (Test-Path $exe) { $relOk++ } else { $relFail++; Write-Output "[FAIL-rel] $t"; Get-Content "build\$t.rc.log" | Select-Object -First 6 }
}
# evict wire (special define)
$src = Join-Path $core "test_evict_wire.c"
$exe = Join-Path $core "build\test_evict_wire.exe"
& cl /nologo /O2 /W4 /utf-8 /I. /DRB_EVICT_HOOK_EXTERNAL $src cache.c /Fe:$exe /Fo:build\ > "build\test_evict_wire.rc.log" 2>&1
if (Test-Path $exe) { $relOk++ } else { $relFail++; Write-Output "[FAIL-rel] test_evict_wire"; Get-Content "build\test_evict_wire.rc.log" | Select-Object -First 6 }

Write-Output "REL_OK=$relOk REL_FAIL=$relFail"
foreach ($t in ($Tests + @("test_evict_wire"))) {
  $exe = Join-Path $core "build\$t.exe"
  if (Test-Path $exe) {
    & $exe > "build\rel_$t.out" 2> "build\rel_$t.err"
    if ($LASTEXITCODE -ne 0) { Write-Output "$t exit=$LASTEXITCODE"; $runFail++ }
  }
}
Write-Output "RUN_FAIL=$runFail"
Write-Output "AUDITFIX_TEST_SUMMARY rel_ok=$relOk rel_fail=$relFail run_fail=$runFail"
