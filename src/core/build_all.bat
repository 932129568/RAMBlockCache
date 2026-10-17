@echo off
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program1\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"
if not exist build mkdir build

set TESTS=test_cache test_counter test_geom test_mixed test_parflush test_regress test_release test_v5 test_v6 test_bigread test_flush_many edge_v3 edge_v4 stress_v3 stress_v4 bench_readhit test_auditfix

echo ==== RELEASE BUILD ====
for %%T in (%TESTS%) do (
  cl /nologo /O2 /W4 /utf-8 /I. %%T.c cache.c /Fe:build\%%T.exe /Fobuild\ > build\%%T.rc.log 2>&1
  if exist build\%%T.exe (echo [OK-rel] %%T) else (echo [FAIL-rel] %%T & type build\%%T.rc.log)
)
rem test_evict_wire：淘汰接线测试。cache.c 以 /DRB_EVICT_HOOK_EXTERNAL 抑制用户态默认空桩，
rem 由测试文件自备 RbFeatOnEvict 捕获桩（RB_WIRED_L2_ON_EVICT=1）。
cl /nologo /O2 /W4 /utf-8 /I. /DRB_EVICT_HOOK_EXTERNAL test_evict_wire.c cache.c /Fe:build\test_evict_wire.exe /Fobuild\ > build\test_evict_wire.rc.log 2>&1
if exist build\test_evict_wire.exe (echo [OK-rel] test_evict_wire) else (echo [FAIL-rel] test_evict_wire & type build\test_evict_wire.rc.log)

echo ==== ASAN BUILD ====
for %%T in (%TESTS%) do (
  cl /nologo /Od /Zi /W4 /utf-8 /I. /fsanitize=address %%T.c cache.c /Fe:build\asan_%%T.exe /Fobuild\ > build\asan_%%T.rc.log 2>&1
  if exist build\asan_%%T.exe (echo [OK-asan] %%T) else (echo [FAIL-asan] %%T & type build\asan_%%T.rc.log)
)
cl /nologo /Od /Zi /W4 /utf-8 /I. /fsanitize=address /DRB_EVICT_HOOK_EXTERNAL test_evict_wire.c cache.c /Fe:build\asan_test_evict_wire.exe /Fobuild\ > build\asan_test_evict_wire.rc.log 2>&1
if exist build\asan_test_evict_wire.exe (echo [OK-asan] test_evict_wire) else (echo [FAIL-asan] test_evict_wire & type build\asan_test_evict_wire.rc.log)
echo ==== DONE ====
