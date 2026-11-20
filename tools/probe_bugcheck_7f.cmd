@echo off
setlocal
chcp 437 >nul
rem ===================================================================
rem  probe_bugcheck_7f.cmd  [296 2026-11-19 v3]
rem
rem  Self-elevating entry for probe_bugcheck_7f.ps1.
rem
rem  Minidump under C:\Windows\Minidump is Administrators-only; an
rem  unelevated cdb returns "Win32 error 0n5 access denied". UAC must be
rem  answered by a human, so this wrapper re-launches itself with
rem  -Verb RunAs and then runs the analysis elevated.
rem
rem  v1 ran -AllDumps, which downloads symbols for 5 dumps in a row.
rem  v2 defaulted to the newest dump only. But v2 still ran
rem  ".reload /f" (force-reload EVERY loaded module) which made cdb
rem  grind through every driver on the box asking a symbol server for
rem  symbols it did not need - 37 minutes, 19 modules, no result.
rem
rem  v3 default path is zero-symbol / zero-network: read _EXCEPTION_RECORD
rem  via dq (P3) and registers via .trap (P2). Seconds, not minutes.
rem
rem  Usage:
rem    probe_bugcheck_7f.cmd            fast: newest dump, no symbol download
rem    probe_bugcheck_7f.cmd full       add !analyze -v + blackbox (needs symbols)
rem    probe_bugcheck_7f.cmd all        every dump (fast mode)
rem
rem  READ-ONLY: no file deletion, no registry write, no driver touch.
rem  Output: C:\ProgramData\rb-deploy\bugcheck-7f-<stamp>.txt
rem
rem  Pure ASCII, no BOM, CRLF.
rem ===================================================================

if /i "%~1"=="elevated" goto work

set "MODE=%~2"
if "%MODE%"=="" set "MODE=newest"

echo [*] requesting elevation (UAC prompt) ...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList 'elevated %~1' -Verb RunAs"
exit /b 0

:work
if "%MODE%"=="" set "MODE=newest"
set "PS1=%~dp0probe_bugcheck_7f.ps1"
if not exist "%PS1%" (
  echo [FAIL] missing: %PS1%
  pause
  exit /b 1
)

set "EXTRA="
if /i "%MODE%"=="all"  set "EXTRA=-AllDumps"
if /i "%MODE%"=="full" set "EXTRA=-Full"
if /i "%MODE%"=="offline" set "EXTRA=-Offline"

echo ============================================================
echo   0x7F double-fault dump triage   mode=%MODE%
echo ============================================================
echo.
echo   Fast mode reads the exception record directly - no symbol
echo   download, no network. Should finish in seconds.
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %EXTRA%
set "RC=%errorlevel%"

echo.
echo ==== exit=%RC% ====
if "%RC%"=="4" echo   NOTE: rc=4 means symbols were missing - stack attribution only.
pause
exit /b %RC%