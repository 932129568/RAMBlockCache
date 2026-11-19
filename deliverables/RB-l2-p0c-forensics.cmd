@echo off
rem ===========================================================================
rem  RB-l2-p0c-forensics.cmd
rem  A-PLAN: second live run with read-only forensics sampling (195/196).
rem
rem  Same test as RB-l2-p0c-test.cmd, PLUS -ProbeStats:
rem    - rbfeatctl stats -json sampled at baseline / post-enable / post-load /
rem      pre+post shrink / every 15s poll / final  -> l2-probe-<utc>.csv
rem      (stats -json for core/L2 counters, text stats for l2state+admitRejects)
rem    - fsutil file queryextents on the load file -> physical LCN -> volume
rem      byte offset, compared against the L2 window (s_Slots * 64KB)
rem
rem  Does NOT change any verdict, exit code, judgement criterion or rollback.
rem  Double-click it. It asks for UAC and re-launches itself elevated.
rem  Default: container C: 2GB, load on D:, 10 minute window, auto-rollback.
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
echo   RAMBlockCache P0-C FORENSICS (A-plan)
echo   container C:  2 GB    load D:    window 10 min
echo   read-only sampling: core/L2 counters + load file physical LBA
echo ==================================================================
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-p0c-test.ps1" -ProbeStats

set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" echo *** PASS - L2 executed and the box is clean ***
if "%RC%"=="1" echo *** FAIL - rolled back, read the log ***
if "%RC%"=="2" echo *** REFUSED at preflight - nothing was changed ***
if "%RC%"=="3" echo *** recovered from crashed run - rolled back, run this again ***
if "%RC%"=="4" echo *** internal error - nothing reliable happened ***
if "%RC%"=="5" echo *** UNCOVERED - stability only, not a caching PASS ***
echo.
echo Forensics CSV: C:\ProgramData\rb-deploy\l2-probe-*.csv
echo.
pause
exit /b %RC%
