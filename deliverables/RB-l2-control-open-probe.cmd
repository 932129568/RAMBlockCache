@echo off
rem ===========================================================================
rem  RB-l2-control-open-probe.cmd
rem
rem  WHY: after the 2026-11-15 J6 prepare + ONE normal reboot, the verifier
rem  reported L2state=0 with zero L2 counters while Features\Flags=0x4, and the
rem  driver log for that boot had NO 'l2:' line at all. Code reading says the
rem  data path cannot perform the FIRST open (rbL2.c:1093 VolId guard vs
rem  s_VolId==0, which is only written inside the open path at rbL2.c:706), so
rem  only a control-path REFRESH can open the container.
rem
rem  This wrapper answers that with ONE observation: send refresh, compare stats
rem  before/after. It is NOT a J6 verdict and it relaxes no criterion.
rem
rem  Safety: flagged as near-read-only.
rem    - NO registry write, NO service control, NO reboot.
rem    - NO driver build / sign / deploy.
rem    - Write-back and WBCOAL stay OFF (R4/R9 red lines untouched).
rem    - The single REFRESH re-reads Features\Flags and performs an idempotent
rem      container open; it does not change any configured value.
rem  Exit: 0 = probe completed / 2 = could not run.
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
echo   L2 CONTROL-PATH OPEN PROBE - elevated
echo   one REFRESH only; no config write; write-back / WBCOAL OFF
echo ==================================================================
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-control-open-probe.ps1"
set "RC=%ERRORLEVEL%"

echo.
echo ==================================================================
echo   probe exit code: %RC%
echo ==================================================================
if not "%RC%"=="0" goto :failed

echo   Next: send this screen back. Expected reading:
echo     - state 0 -^> 1 and L2Miss grows  =^> data path could not open it,
echo       control path could; fix surface is small (rbL2.c VolId guard).
echo     - state stays 0                   =^> a different gate; driver log
echo       should now show an 'l2:' refusal line.
echo     - index unchanged                 =^> nothing to load = cold start,
echo       so cross-boot hits are impossible until shutdown finalises .idx.
echo.
echo   Restore default posture afterwards:
echo     deliverables\RB-l2-p0c-rollback.cmd
echo.
pause
exit /b 0

:failed
echo *** probe could not run (exit=%RC%). Nothing was changed. ***
echo *** Run it by double-clicking this file, not from a restricted shell. ***
echo.
pause
exit /b 2
