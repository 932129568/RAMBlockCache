@echo off
rem ===========================================================================
rem  RB-j6-deploy-guard-fix.cmd
rem  Deploy the FIXED boot_guard.ps1 to the live runtime location and re-arm
rem  the volume class UpperFilters. User-approved 2026-11-13 (path C).
rem  Self-elevates ONCE via UAC.
rem
rem  WHY: the earlier fix only changed src/scripts/boot_guard.ps1, but the
rem  scheduled task runs C:\ProgramData\RAMBoost\boot_guard.ps1 (deploy.ps1:497
rem  is the copy point). The live copy stayed old and kept stripping the
rem  filter on every healthy-disarmed boot.
rem
rem  FAIL-CLOSED: verifies running sys sha / Authenticode / BOOT_START and
rem  that the repo copy really contains the DeployAck guard before writing
rem  anything. Backs up the live guard and the old UpperFilters, then reads
rem  every write back. Log has the exact rollback instructions.
rem
rem  NO sys replacement, NO sign, NO anchor change, NO reboot, NO write-back
rem  or WBCOAL change, NO method=2, NO bootcfg POST, NO verifier /reset.
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
set "PS1=%REPO%\src\scripts\j6_deploy_guard_fix.ps1"

echo ==================================================================
echo   RAMBlockCache J6 GUARD FIX - elevated
echo   deploy fixed boot_guard.ps1 + re-arm UpperFilters
echo ==================================================================
echo.
"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
set "RC=%ERRORLEVEL%"
echo.
echo ==================================================================
if "%RC%"=="0" (echo *** GUARD FIX OK ***) else (echo *** FAILED - see log ***)
echo ==================================================================
echo log: C:\ProgramData\rb-deploy\j6-deploy-guard-fix.log
echo.
pause
exit /b %RC%
