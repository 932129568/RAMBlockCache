@echo off
rem ===========================================================================
rem  RB-r2-restore-step1.cmd
rem  STEP 1 of 2: close the Phase 4 / Path B R2 exception window.
rem  Origin: HANDOVER-CURRENT.md sec 2 step 1; 239 sec 4.1; 240 sec 5.1.
rem
rem  Current live System32 image is the unanchored test build ae47113c...,
rem  while dist and the three anchors point to the signed production image
rem  7125d219.... The 09:30 observation watch will be RED until this is fixed.
rem
rem  This step calls phase4-pathB.ps1 -Step Restore -AcceptR8Exception.
rem  It does NOT touch dist, the three anchors, the watch pin, or git.
rem  It does NOT reboot. One UAC prompt only.
rem
rem  NEXT (user action): reboot this machine ONCE, then double-click
rem  RB-r2-restore-step2.cmd for VerifyRestore.
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"
set "STAGE=C:\ProgramData\rb-deploy\phase4-stage"
set "RUNNER=%STAGE%\phase4-pathB.ps1"
set "LOG=C:\ProgramData\rb-deploy\r2-restore-step1.log"
for %%I in ("%~dp0..") do set "REPO=%%~fI"
set "RB_SELF=%~f0"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting one UAC prompt ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:RB_SELF -Verb RunAs"
    exit /b 0
)

if not exist "%RUNNER%" goto :no_runner
if not exist "C:\ProgramData\rb-deploy" mkdir "C:\ProgramData\rb-deploy"

echo ==================================================================
echo   R2 RESTORE STEP 1 - elevated
echo ==================================================================
echo repo : %REPO%
echo log  : %LOG%
echo.
"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%RUNNER%" -Step Restore -Repo "%REPO%" -AcceptR8Exception > "%LOG%" 2>&1
set "RC=%ERRORLEVEL%"
type "%LOG%"
echo.
echo RESTORE_EXIT = %RC%
echo NEXT: reboot this machine ONCE, then run RB-r2-restore-step2.cmd
echo Send %LOG% back together with the step 2 log.
pause
exit /b %RC%

:no_runner
echo [STOP] runner not found: %RUNNER%
echo The Path B staging directory must already exist on this machine.
pause
exit /b 2
