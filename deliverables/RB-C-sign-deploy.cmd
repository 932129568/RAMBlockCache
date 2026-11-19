@echo off
rem ===========================================================================
rem  RB-C-sign-deploy.cmd
rem  C-plan (J18): sign + deploy the hashed-slot driver in ONE elevated session.
rem
rem  Preconditions (already done by the agent):
rem    - P1 source changes + P2 build: src\driver\x64\Release\RAMBlockCache.sys
rem      sha256 = 8d70f541739d577b5b0cb4a3e611d601c16ab7cfd402f39caad4e910ce1f51b2
rem    - core 66 regression RUN_FAIL=0, gates 20/20 ALL GATES PASS
rem    - L2 static contract gate (src\scripts\check_l2_contract.py) PASS
rem
rem  This script does NOT reboot. After it prints "DEPLOYED", reboot once,
rem  then tell the agent to run P5 acceptance.
rem
rem  WARNING: do NOT run this before step P3 is intended. It replaces the
rem  currently deployed VALID driver e3f6005d... with the C-plan build.
rem  Rollback instructions: deliverables\206-l2-c-plan-deploy-runbook-2026-11-12.md
rem ===========================================================================
setlocal

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting UAC ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

echo ==================================================================
echo   RAMBlockCache C-PLAN : SIGN + DEPLOY  (elevated)
echo   new driver : 8d70f541...  (188928 bytes, currently UNSIGNED)
echo   replaces   : e3f6005d...  (196936 bytes, Valid)
echo ==================================================================
echo.

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-C-sign-deploy.ps1"

set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (
  echo *** OK: signed, anchors synced, deployed. NO REBOOT PERFORMED. ***
  echo *** Next: reboot once, then tell the agent for P5 acceptance.  ***
) else (
  echo *** FAILED (exit %RC%^). Do NOT reboot or continue. ***
  echo *** Send %TEMP%\rb-C-sign-deploy.result and .log to the agent. ***
)
echo.
echo result: %TEMP%\rb-C-sign-deploy.result
echo log   : %TEMP%\rb-C-sign-deploy.log
echo.
pause
exit /b %RC%
