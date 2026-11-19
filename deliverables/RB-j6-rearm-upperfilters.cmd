@echo off
rem ===========================================================================
rem  RB-j6-rearm-upperfilters.cmd
rem  Restore the volume-class UpperFilters reference removed by boot_guard.ps1.
rem  User-approved 2026-11-13 (path C). Self-elevates ONCE via UAC.
rem
rem  The PowerShell side is FAIL-CLOSED: it verifies the running sys hash,
rem  Authenticode status and BOOT_START before touching any registry value,
rem  backs up the old UpperFilters value into the log, and reads every write
rem  back. If any preflight or readback fails, nothing is changed.
rem
rem  NO sys replacement, NO sign, NO anchor change, NO reboot, NO write-back
rem  or WBCOAL change, NO method=2, NO bootcfg POST, NO verifier /reset.
rem  Reboot is a SEPARATE user action after this succeeds.
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting UAC ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

set "REPO=%~dp0.."
set "PS1=%REPO%\src\scripts\j6_rearm_upperfilters.ps1"

echo ==================================================================
echo   RAMBlockCache J6 REARM - elevated
echo   restoring volume class UpperFilters only
echo ==================================================================
echo.
"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
set "RC=%ERRORLEVEL%"
echo.
echo ==================================================================
if "%RC%"=="0" (echo *** REARM OK ***) else (echo *** REARM FAILED - see log ***)
echo ==================================================================
echo log: C:\ProgramData\rb-deploy\j6-rearm-upperfilters.log
echo NEXT: reboot ONCE from the Start menu, then double-click
echo       RB-j6-postboot-probe.cmd again to confirm volumes are present.
echo.
pause
exit /b %RC%
