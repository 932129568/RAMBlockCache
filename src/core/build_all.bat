@echo off
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program1\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"
if not exist build mkdir build

rem [FIX QA5-1/QA5-6] Judgement by cl exit code (not exe existence -- a stale exe
rem masked a real LNK1104 as [OK] on 2026-10-21). Stale output deleted before link.
rem [FIX QA5-6b] test_evict_wire is NOT in the plain list: without
rem /DRB_EVICT_HOOK_EXTERNAL it collides with cache.c's own RbFeatOnEvict stub
rem (LNK2005/LNK1169). It is built only with the external-hook define below,
rem and run_all.bat runs that exe. Single run-list source lives in run_all.bat.
set TESTS=test_cache test_counter test_geom test_mixed test_parflush test_regress test_release test_v5 test_v6 test_bigread test_flush_many edge_v3 edge_v4 stress_v3 stress_v4 bench_readhit test_auditfix test_write_ex test_wsconc

rem [FIX QA5-1] Judgement by cl exit code, not exe existence:
rem a stale exe from an earlier build masked a real LNK1104 link failure as
rem [OK] (test_wsconc Release link actually failed 2026-10-21 07:14).
rem Also delete stale output before linking so success means THIS build.

set BUILDFAIL=0

echo ==== RELEASE BUILD ====
for %%T in (%TESTS%) do (
  if exist build\%%T.exe del /q build\%%T.exe >nul 2>&1
  cl /nologo /O2 /W4 /utf-8 /I. %%T.c cache.c /Fe:build\%%T.exe /Fobuild\ > build\%%T.rc.log 2>&1
  if errorlevel 1 (echo [FAIL-rel] %%T & type build\%%T.rc.log & set /a BUILDFAIL+=1)
  if exist build\%%T.exe (echo [OK-rel] %%T) else (set /a BUILDFAIL+=1)
)

echo ==== ASAN BUILD ====
for %%T in (%TESTS%) do (
  if exist build\asan_%%T.exe del /q build\asan_%%T.exe >nul 2>&1
  cl /nologo /Od /Zi /W4 /utf-8 /I. /fsanitize=address %%T.c cache.c /Fe:build\asan_%%T.exe /Fobuild\ > build\asan_%%T.rc.log 2>&1
  if errorlevel 1 (echo [FAIL-asan] %%T & type build\asan_%%T.rc.log & set /a BUILDFAIL+=1)
  if exist build\asan_%%T.exe (echo [OK-asan] %%T) else (set /a BUILDFAIL+=1)
)

rem [FIX QA5-6] Single source of truth: test_evict_wire is now inside TESTS
rem above (needs /DRB_EVICT_HOOK_EXTERNAL -- handled by the dedicated lines below).
rem Rebuild it with the special define in both modes (overwrites the plain build).
cl /nologo /O2 /W4 /utf-8 /I. /DRB_EVICT_HOOK_EXTERNAL test_evict_wire.c cache.c /Fe:build\test_evict_wire.exe /Fobuild\ > build\test_evict_wire.rc.log 2>&1
if errorlevel 1 (echo [FAIL-rel] test_evict_wire & type build\test_evict_wire.rc.log & set /a BUILDFAIL+=1)
if exist build\test_evict_wire.exe (echo [OK-rel] test_evict_wire ^(external hook^)) else (set /a BUILDFAIL+=1)
cl /nologo /Od /Zi /W4 /utf-8 /I. /fsanitize=address /DRB_EVICT_HOOK_EXTERNAL test_evict_wire.c cache.c /Fe:build\asan_test_evict_wire.exe /Fobuild\ > build\asan_test_evict_wire.rc.log 2>&1
if errorlevel 1 (echo [FAIL-asan] test_evict_wire & type build\asan_test_evict_wire.rc.log & set /a BUILDFAIL+=1)
if exist build\asan_test_evict_wire.exe (echo [OK-asan] test_evict_wire ^(external hook^)) else (set /a BUILDFAIL+=1)

rem [FIX QA5-1/9] Aggregate exit code: any failure -> exit 1 (fail-closed).
echo ==== DONE buildfail=%BUILDFAIL% ====
if %BUILDFAIL% gtr 0 exit /b 1
exit /b 0
