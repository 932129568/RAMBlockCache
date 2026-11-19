@echo off
rem ===========================================================================
rem  RB-deploy-driver.cmd   [one double-click]
rem
rem  WHY (report 253, 2026-11-15): the signed patch binary (report 252) sits in
rem  dist\ with all three anchors pointing at it, but the running driver is
rem  still the 7125d219 build. deploy.ps1 is the real deployment and needs
rem  elevation; this wrapper adds a readable log and a pre-change gate.
rem
rem  What it does:
rem    1. self-elevates (UAC once)
rem    2. gate: dist hash == deploy.ps1 anchor == verify anchor == sidecar,
rem       and the signature must be Valid. Nothing is touched if the gate fails.
rem    3. runs deploy.ps1 in a child process (it ends with exit)
rem    4. prints a post-deploy snapshot and keeps the transcript at
rem       C:\ProgramData\rb-deploy\deploy-apply-<utc>.log
rem
rem  Safety:
rem    - Does NOT reboot. The reboot is an explicit human action.
rem    - deploy.ps1 keeps its own rollback: on any failure it restores
rem      UpperFilters / service config and exits non-zero.
rem    - Features\Flags is forced to 0 by deploy.ps1 (all feature flags off).
rem
rem  Exit: 0 = deployed, reboot pending / 1 = deploy failed and rolled back /
rem        2 = refused before any change.
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

echo ==================================================================
echo   RAMBlockCache DEPLOY - elevated
echo   gate -^> deploy.ps1 -^> snapshot; NO reboot here
echo ==================================================================
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\src\scripts\deploy_apply.ps1"
set "RC=%ERRORLEVEL%"

echo.
echo ==================================================================
echo   deploy-apply exit code: %RC%
echo ==================================================================
if "%RC%"=="0" goto :ok
if "%RC%"=="2" goto :refused

echo *** deploy FAILED (exit=%RC%). deploy.ps1 ran its own rollback. ***
echo *** Nothing is rebooted. Read the log above before retrying. ***
echo *** Worst case (boot loop after an earlier attempt): ***
echo ***   src\scripts\offline_rollback.cmd from WinRE ***
echo.
pause
exit /b 1

:refused
echo *** refused before any change (gate or environment). ***
echo *** Look for the [FAIL] lines above. ***
echo.
pause
exit /b 2

:ok
echo   Deployed. The System32 copy activates on the NEXT REBOOT.
echo.
echo   Then run:  deliverables\RB-postreboot-verify.cmd
echo   Expected:  PASS=8 FAIL=0, UpperFilters keeps RAMBlockCache, volumes ^> 0
echo.
pause
exit /b 0
