@echo off
rem ===========================================================================
rem  RB-l2-p1-prepare.cmd   [check | run]
rem
rem  Report 255 section 7, stage A. Arms the L2 window so that the NEXT boot's
rem  first data read is a clean discriminator for the P1 patch (rbL2.c VolId
rem  guard). rbfeatctl l2 enable sends its OWN REFRESH (rbfeatctl.c:271), so
rem  that open is PREPARATION, not a verdict -- a normal reboot is required.
rem
rem  THIS WRAPPER DOES CHANGE STATE (only after a passing pre-flight):
rem    - creates D:\rbc-l2-load.bin (128 MiB)
rem    - rbfeatctl l2 enable C 2   -> Features\Flags |= 4, container + index
rem    - writes C:\ProgramData\rb-deploy\l2-p1-baseline.json
rem  It does NOT deploy, sign, reboot, or touch write-back / WBCOAL.
rem
rem  "check" = strictly read-only pre-flight, nothing is written anywhere
rem            except the script's own log under C:\ProgramData\rb-deploy.
rem
rem  AFTER THIS: NORMAL restart from the Start menu (never force power off),
rem  then RB-l2-p1-verify.cmd, then RB-l2-p0c-rollback.cmd before 11-16 09:30.
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
echo   RAMBlockCache L2 P1 PREPARE - elevated
echo   mode=%MODE% ; arms Flags=4 window ; NO deploy / NO reboot here
echo ==================================================================
echo.

set "PYTHONUTF8=1"
set "SCRIPT=%~dp0..\src\scripts\l2_p1_prepare.py"
if "%MODE%"=="check" (
    %PY% "%SCRIPT%" --check
) else (
    %PY% "%SCRIPT%"
)
set "RC=%ERRORLEVEL%"

echo.
echo ==================================================================
echo   prepare exit code: %RC%
echo ==================================================================
if "%MODE%"=="check" goto :done

if "%RC%"=="0" (
    echo   Window is ARMED. Next:
    echo     1^) NORMAL restart from the Start menu ^(never force power off^)
    echo     2^) deliverables\RB-l2-p1-verify.cmd
    echo     3^) MANDATORY before 11-16 09:30: deliverables\RB-l2-p0c-rollback.cmd
) else (
    echo   *** Prepare did NOT complete. Nothing else was changed. ***
    echo   *** If any step ran, close the window with:             ***
    echo   ***   deliverables\RB-l2-p0c-rollback.cmd                  ***
)

:done
echo.
pause
exit /b %RC%
