@echo off
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program1\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"
rem [FIX #3] QA artifact freshness pre-check: source newer than artifact
rem (= last change uncovered) aborts before the test loop, same fail-closed
rem discipline as QA5-1 exit-code aggregation.
python ..\scripts\check_repo.py --pre > build\check_repo_pre.out 2>&1
if errorlevel 1 (
    echo [X] check_repo pre-check FAILED - stale artifacts, see build\check_repo_pre.out
    type build\check_repo_pre.out
    exit /b 1
)
rem [FIX QA5-6] Single list source (same as build_all.bat): includes test_evict_wire.
set TESTS=test_cache test_counter test_geom test_mixed test_parflush test_regress test_release test_v5 test_v6 test_bigread test_flush_many edge_v3 edge_v4 stress_v3 stress_v4 bench_readhit test_auditfix test_write_ex test_wsconc test_evict_wire

set RUNFAIL=0

echo ===== RELEASE =====
for %%T in (%TESTS%) do (
  build\%%T.exe > build\rel_%%T.out 2> build\rel_%%T.err
  set ec=!ERRORLEVEL!
  echo %%T exit=!ec!
  if !ec! neq 0 set /a RUNFAIL+=1
)
echo ===== ASAN =====
for %%T in (%TESTS%) do (
  build\asan_%%T.exe > build\asan_%%T.out 2> build\asan_%%T.err
  set ec=!ERRORLEVEL!
  findstr /C:"AddressSanitizer" build\asan_%%T.out build\asan_%%T.err > build\asan_%%T.asan 2>&1
  if !ERRORLEVEL! equ 0 (echo %%T ASAN-REPORT exit=!ec! & set /a RUNFAIL+=1) else (echo %%T exit=!ec!)
  if !ec! neq 0 set /a RUNFAIL+=1
)
echo ===== DONE runfail=%RUNFAIL% =====
rem [FIX QA5-9] Aggregate exit code: any nonzero test exit or ASan report -> exit 1.
if %RUNFAIL% gtr 0 exit /b 1
exit /b 0
