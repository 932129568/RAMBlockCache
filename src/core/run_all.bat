@echo off
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program1\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"
set TESTS=test_cache test_counter test_geom test_mixed test_parflush test_regress test_release test_v5 test_v6 test_bigread test_flush_many edge_v3 edge_v4 stress_v3 stress_v4 bench_readhit

echo ===== RELEASE =====
for %%T in (%TESTS%) do (
  build\%%T.exe > build\rel_%%T.out 2> build\rel_%%T.err
  echo %%T exit=!ERRORLEVEL!
)
echo ===== ASAN =====
for %%T in (%TESTS%) do (
  build\asan_%%T.exe > build\asan_%%T.out 2> build\asan_%%T.err
  set ec=!ERRORLEVEL!
  findstr /C:"AddressSanitizer" build\asan_%%T.out build\asan_%%T.err > build\asan_%%T.asan 2>&1
  if !ERRORLEVEL! equ 0 (echo %%T ASAN-REPORT exit=!ec!) else (echo %%T exit=!ec!)
)
echo ===== DONE =====
