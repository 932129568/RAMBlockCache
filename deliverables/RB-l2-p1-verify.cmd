@echo off
rem ===========================================================================
rem  RB-l2-p1-verify.cmd   [check | run]
rem
rem  Report 255 section 7, stage B. Run this FIRST after the normal reboot that
rem  follows RB-l2-p1-prepare.cmd. STRICTLY READ-ONLY with respect to driver
rem  state: NO refresh, NO l2 enable/disable, NO registry write, NO deletion,
rem  NO reboot, NO rollback. The only files written are its own log and the
rem  evidence JSON under C:\ProgramData\rb-deploy.
rem
rem  ORDER IS THE DISCRIMINATOR: the .idx header, the pwr: power-IRP lines and
rem  pre-read stats are captured BEFORE the unbuffered D: read, because that
rem  read may open the container and rewrite .idx.
rem
rem  Four verdicts from one boot:
rem    P1 data-path first open   P2 power-IRP visibility
rem    B2 shutdown index save   J6 cross-boot hit (evidence only)
rem
rem  Exit: 0 = P1 PASS / 1 = P1 FAIL / 5 = INCONCLUSIVE / 2 = could not run.
rem  INCONCLUSIVE is NOT a driver defect.
rem
rem  MANDATORY CLEANUP before 11-16 09:30: RB-l2-p0c-rollback.cmd
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
    echo *** python 3 not found on PATH; this probe needs the stdlib only. ***
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
echo   RAMBlockCache L2 P1 VERIFY - elevated, near read-only
echo   mode=%MODE% ; NO refresh / NO config write / NO rollback
echo ==================================================================
echo.

set "PYTHONUTF8=1"
set "SCRIPT=%~dp0..\src\scripts\l2_p1_verify.py"
if "%MODE%"=="check" (
    %PY% "%SCRIPT%" --check
) else (
    %PY% "%SCRIPT%"
)
set "RC=%ERRORLEVEL%"

echo.
echo ==================================================================
echo   verify exit code: %RC%
echo ==================================================================
echo   0 = P1 PASS   1 = P1 FAIL   5 = INCONCLUSIVE   2 = could not run
echo.
echo   MANDATORY cleanup before 11-16 09:30:
echo     deliverables\RB-l2-p0c-rollback.cmd
echo   ^(clears Flags / L2Letter and deletes container, index, load^)
echo   The evidence JSON was saved first, so cleanup cannot hide the result.
echo.
pause
exit /b %RC%
