@echo off
rem ===========================================================================
rem  RB-slice-sign-deploy.cmd
rem  C-SLICE (J18): sign + deploy the hashed-slot driver in ONE elevated session.
rem
rem  Preconditions (already done by the agent):
rem    - P1 source changes + P2 build: src\driver\x64\Release\RAMBlockCache.sys
rem      sha256 = cba941b3ca774fe70aa2f61279a8a63c1f345ea8d4ee4f444c4c600c24f2dd78
rem    - core 66 regression RUN_FAIL=0, gates 20/20 ALL GATES PASS
rem    - L2 static contract gate (src\scripts\check_l2_contract.py) PASS
rem
rem  This script does NOT reboot. After it prints "DEPLOYED", reboot once,
rem  then tell the agent to run P6 acceptance.
rem
rem  WARNING: do NOT run this before step P3 is intended. It replaces the
rem  currently deployed VALID driver 37700a686... with the C-SLICE build.
rem  Rollback instructions: deliverables\218-l2-slice-design-2026-11-13.md
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
echo   RAMBlockCache C-SLICE : SIGN + DEPLOY  (elevated)
echo   new driver : cba941b3...  (189952 bytes, currently UNSIGNED)
echo   replaces   : 37700a686...  (196936 bytes, Valid)
echo ==================================================================
echo.

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-slice-sign-deploy.ps1"

set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (
  echo *** OK: signed, anchors synced, deployed. NO REBOOT PERFORMED. ***
  echo *** Next: reboot once, then tell the agent for P6 acceptance.  ***
) else (
  echo *** FAILED (exit %RC%^). Do NOT reboot or continue. ***
  echo *** Send %TEMP%\rb-slice-sign-deploy.result and .log to the agent. ***
)
echo.
echo result: %TEMP%\rb-slice-sign-deploy.result
echo log   : %TEMP%\rb-slice-sign-deploy.log
echo.
pause
exit /b %RC%
