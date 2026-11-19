@echo off
rem ===========================================================================
rem  RB-phase4-close-race-verify.cmd
rem  Phase 4 / C-02 long-run close-race evidence collector. READ ONLY. One UAC.
rem
rem  Usage:
rem    RB-phase4-close-race-verify.cmd                 -> 30 min, read only
rem    RB-phase4-close-race-verify.cmd 60             -> 60 min, read only
rem    RB-phase4-close-race-verify.cmd 30 INJECT      -> 30 min + rbfeatctl
rem                                                   refresh interleaving
rem
rem  INJECT is for an ISOLATED test window ONLY. It sends a control-plane
rem  REFRESH IOCTL; it does not change Features\Flags, write-back or WBCOAL.
rem
rem  This script NEVER deploys, signs, reboots, writes registry, changes
rem  config, syncs anchors or resets the observation watch pin. The patched
rem  test build at C:\ProgramData\rb-deploy\phase4-test\ is NOT installed here.
rem
rem  Verifier is already active on this machine (0x001209BB, Special Pool
rem  + Pool Tracking, target RAMBlockCache.sys), so no extra reboot is needed
rem  to enable it. See deliverables/design/phase4-close-race-verification-2026-11-13.md
rem
rem  Log: C:\ProgramData\rb-deploy\phase4-close-race-YYYYMMDDTHHMMSSZ.log
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"
set "RB_REPO=%~dp0.."
set "PROBE=%~dp0..\src\scripts\phase4_close_race_probe.ps1"
set "MINUTES=30"
if not "%~1"=="" set "MINUTES=%~1"
set "INJECT="
if /I "%~2"=="INJECT" set "INJECT=-InjectRefresh"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting UAC ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '%MINUTES%','%~2' -Verb RunAs"
    exit /b 0
)

echo ==================================================================
echo   RAMBlockCache Phase 4 CLOSE-RACE EVIDENCE PROBE - elevated, READ ONLY
echo   minutes : %MINUTES%
echo   inject  : %INJECT%
echo ==================================================================
echo.
if not exist "%PROBE%" goto :no_probe

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%PROBE%" -Minutes %MINUTES% -IntervalSec 5 %INJECT%
set "RC=%ERRORLEVEL%"
echo.
echo ==================================================================
echo   PROBE EXIT = %RC%
echo ==================================================================
echo log: C:\ProgramData\rb-deploy\phase4-close-race-YYYYMMDDTHHMMSSZ.log
echo   (exact path printed above by the elevated PowerShell step)
echo.
echo Send the log plus driver-YYYYMMDD.log back for the verdict.
echo NO closure is claimed by this script (red line R7).
echo.
pause
exit /b %RC%

:no_probe
echo *** MISSING PROBE *** %PROBE%
echo Nothing was changed.
pause
exit /b 2
