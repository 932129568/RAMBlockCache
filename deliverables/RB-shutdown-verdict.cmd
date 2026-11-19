@echo off
rem ===========================================================================
rem  RB-shutdown-verdict.cmd   [check | run]
rem
rem  Report 267. Run this ONCE after all shutdown/reboot rounds are done.
rem  It is the only command needed after the power cycles. STRICTLY READ-ONLY
rem  with respect to the machine: NO refresh, NO config write, NO registry
rem  write, NO deletion, NO reboot, NO rollback. The only files written are
rem  its own evidence JSON + summary TXT under C:\ProgramData\rb-deploy.
rem
rem  It aggregates in one pass:
rem    - registry DiagShutdownSeq + shutdown-sentinel.bin  (three-way verdict)
rem    - Windows System events 1074 / 109 / 13 -> action attribution
rem    - driver log 'diag: IRP_MJ_SHUTDOWN seen' lines (best-effort only)
rem    - HiberbootEnabled (was hybrid shutdown even possible)
rem
rem  Exit: 0 = delivered + shutdown file I/O works
rem        3 = no delivery evidence
rem        4 = partial / inconsistent (needs human review)
rem        2 = could not run
rem
rem  'check' is a fast read-only dry run that writes NO report files.
rem ===========================================================================
setlocal

set "MODE=%~1"
if "%MODE%"=="" set "MODE=run"

set "PY="
where py >nul 2>&1 && set "PY=py -3"
if not defined PY (
    where python >nul 2>&1 && set "PY=python"
)
if not defined PY (
    echo *** python 3 not found on PATH; this tool needs the stdlib only. ***
    pause
    exit /b 2
)

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting UAC ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs -ArgumentList '%MODE%'"
    exit /b 0
)

echo ==================================================================
echo   RAMBlockCache SHUTDOWN VERDICT - elevated, read-only
echo   mode=%MODE% ; NO config write / NO rollback / NO reboot
echo ==================================================================
echo.

set "PYTHONUTF8=1"
set "SCRIPT=%~dp0..\src\scripts\shutdown_experiment_report.py"
if "%MODE%"=="check" (
    %PY% "%SCRIPT%" --check
) else (
    %PY% "%SCRIPT%"
)
set "RC=%ERRORLEVEL%"

echo.
echo ==================================================================
echo   shutdown-verdict exit code: %RC%
echo ==================================================================
echo   0 = delivered + file I/O OK   3 = no evidence   4 = partial
echo.
echo   Evidence files are in C:\ProgramData\rb-deploy\shutdown-verdict-*
echo   Just tell me "verdict ready"; no screenshot or command needed.
echo.
pause
exit /b %RC%
