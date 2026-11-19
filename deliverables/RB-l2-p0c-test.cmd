@echo off
rem ===========================================================================
rem  RB-l2-p0c-test.cmd
rem  P0-C: enable L2 on ONE volume, run a bounded load, observe, auto-rollback.
rem
rem  Double-click it. It asks for UAC and re-launches itself elevated.
rem  Default: container C: 2GB, load on D:, 10 minute window, rollback at the end.
rem  TOPOLOGY: the letter in "l2 enable X:" is the CONTAINER volume, not the
rem  accelerated one. The container volume is refused for acceleration, so it must
rem  differ from the load volume. Same-letter is self-referential.
rem
rem  Exit codes: 0 PASS / 1 FAIL (rolled back) / 2 preflight refused /
rem              3 recovered from crashed run (rolled back, re-run) / 4 internal
rem  Log: C:\ProgramData\rb-deploy\l2-p0c-<utc>.log
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
echo   RAMBlockCache P0-C: L2 enable + observe + auto-rollback
echo   container C:  2 GB    load D:    window 10 min
echo ==================================================================
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-p0c-test.ps1"

set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" echo *** PASS - L2 executed and the box is clean ***
if "%RC%"=="1" echo *** FAIL - rolled back, read the log ***
if "%RC%"=="2" echo *** REFUSED at preflight - nothing was changed ***
if "%RC%"=="3" echo *** recovered a crashed run - rolled back, run this again ***
if "%RC%"=="4" echo *** internal error - nothing reliable happened ***
if "%RC%"=="5" echo *** UNCOVERED - L2 opened, no store executed; stability only, not a caching PASS ***
echo.
pause
exit /b %RC%
