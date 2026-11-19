@echo off
rem ===========================================================================
rem  RB-l2-store-warmup.cmd   [stores|hits]
rem
rem  WHY (report 249, 2026-11-15): the 14:48Z control-path probe proved the data
rem  path cannot perform the FIRST L2 open (rbL2.c:1093 VolId guard vs
rem  s_VolId==0, written only inside the open path at rbL2.c:706). With the
rem  container now open, the store path can be exercised without changing any
rem  configuration: read the J6 load file with FILE_FLAG_NO_BUFFERING and
rem  compare rbfeatctl stats before/after.
rem
rem  Two runs make sense:
rem    stores  (default, same boot, container already open)
rem              criterion dL2Stores > 0  => store path works
rem    hits     (first run after a NORMAL reboot + one control-path open)
rem              criterion dL2Hits > 0    => cross-boot hit observed
rem
rem  Safety: read-only on the volume.
rem    - NO registry write, NO service control, NO reboot, NO config change.
rem    - The load file is opened GENERIC_READ only, never written.
rem    - Write-back and WBCOAL stay OFF (R4/R9 red lines untouched).
rem  Exit: 0 = expectation met / 1 = not met / 2 = could not run.
rem ===========================================================================
setlocal

set "MODE=%~1"
if "%MODE%"=="" set "MODE=stores"

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"
set "PY="
where py >nul 2>&1 && set "PY=py -3"
if not defined PY (
    where python >nul 2>&1 && set "PY=python"
)
if not defined PY (
    echo *** python not found on PATH; this probe needs stdlib python 3. ***
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
echo   L2 STORE-SIDE WARM-UP - elevated, read-only on the volume
echo   mode=%MODE%; no config write; write-back / WBCOAL OFF
echo ==================================================================
echo.

set "PYTHONUTF8=1"
%PY% "%~dp0..\src\scripts\l2_store_warmup.py" --expect %MODE%
set "RC=%ERRORLEVEL%"

echo.
echo ==================================================================
echo   warm-up exit code: %RC%
echo ==================================================================
if "%RC%"=="0" goto :ok
if "%RC%"=="1" goto :notmet

echo *** could not run (exit=%RC%). Nothing was changed. ***
echo *** Double-click this file instead of running it from a restricted shell. ***
echo.
pause
exit /b 2

:notmet
echo   The expectation was NOT met. The printed deltas say why:
echo     dCoreMiss = 0        =^> reads never reached the filter (try after a reboot)
echo     state != 1           =^> run RB-l2-control-open-probe.cmd first
echo     stores = hits = 0    =^> L2 unusable in this state; send the driver log
echo.
echo   Restore default posture: deliverables\RB-l2-p0c-rollback.cmd
echo.
pause
exit /b 1

:ok
echo   Expectation met. Next step for a cross-boot hit:
echo     1) NORMAL restart from the Start menu (never force power off / long-press)
echo     2) deliverables\RB-l2-control-open-probe.cmd      (prints .idx BEFORE the
echo        refresh - that line says whether the shutdown finalised the index)
echo     3) this file again with the argument: hits
echo.
echo   Both .idx lines plus the deltas are the evidence for report 249.
echo   Restore default posture afterwards: deliverables\RB-l2-p0c-rollback.cmd
echo.
pause
exit /b 0
