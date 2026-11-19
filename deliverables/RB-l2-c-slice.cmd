@echo off
rem ===========================================================================
rem  RB-l2-c-slice.cmd
rem  C-SLICE P6 acceptance: enable L2 on ONE volume, observe, auto-rollback.
rem
rem  Double-click it ONCE. It asks for UAC and re-launches itself elevated.
rem  Runs RB-l2-p0c-test.ps1 with -ProbeStats -PostC -P5B -SliceTest.
rem
rem  What is new vs RB-l2-c-p5.cmd (stage D only):
rem    Stage C issues 2048 separate 64KB reads -> all take the UNCHANGED
rem    single-block path. It cannot exercise the new slice code.
rem    Stage D issues ONE 1MB read covering 16 consecutive stored blocks:
rem    a >64KB miss range that the old gate rejected outright and the slice
rem    patch must split into 16 pieces (expect L2Hits +16, L2Miss +0).
rem
rem  All read-only. No driver change, no re-sign, no reboot at run time.
rem
rem  PRECONDITION: P6 deploy + ONE reboot already done. The kernel MUST be
rem  running the C-SLICE signed driver: sha256 in the dist sidecar file,
rem  Authenticode Valid, three anchors in sync. This wrapper cannot prove the
rem  reboot - confirm it yourself (boot time later than the deploy time).
rem
rem  Default: container C: 2GB, load on D:, 10 minute window, auto-rollback.
rem  Expected runtime: about 11 to 12 minutes. Keep the window open.
rem
rem  Exit codes: 0 PASS / 1 FAIL - rolled back / 2 preflight refused /
rem              3 recovered from crashed run - rolled back, re-run /
rem              4 internal error / 5 UNCOVERED - stability only, NOT a PASS
rem  Log : C:\ProgramData\rb-deploy\l2-p0c-*.log
rem  CSV : C:\ProgramData\rb-deploy\l2-probe-*.csv
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
echo   RAMBlockCache C-SLICE P6 ACCEPTANCE  - elevated
echo   container C:  2 GB    load D:    window 10 min
echo   switches: -ProbeStats -PostC -P5B -SliceTest
echo ==================================================================
echo.
echo PRECONDITION: P6 deploy done AND the machine rebooted ONCE already.
echo Kernel must be the C-SLICE signed driver - three anchors in sync.
echo This wrapper cannot prove the reboot - confirm it before continuing.
echo Expected runtime: about 11 to 12 minutes. Do NOT close this window.
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-p0c-test.ps1" -ProbeStats -PostC -P5B -SliceTest

set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" echo *** PASS - slice path executed, box clean, rolled back to known state ***
if "%RC%"=="1" echo *** FAIL - rolled back, read the log ***
if "%RC%"=="2" echo *** REFUSED at preflight - nothing was changed ***
if "%RC%"=="3" echo *** RECOVERED a crashed run - rolled back, run this again ***
if "%RC%"=="4" echo *** INTERNAL ERROR - nothing reliable happened ***
if "%RC%"=="5" echo *** UNCOVERED - L2 opened, no store executed, stability only, NOT a caching PASS ***
echo.
echo ------------------------------------------------------------------
echo STAGE D is the only new evidence in this round:
echo   one 1MB read over 16 consecutive stored blocks must give
echo   dL2h = 16 and dL2m = 0 at the post-cslice-span sample.
echo   dL2h = 0 means the range never reached the slice path: wrong
echo   binary still loaded, gate not relaxed, or blocks not indexed.
echo   Everything else (J1/J2/J4/J6, byte-for-byte) is unchanged and
echo   still needs p5_verdict.py plus its own boundaries.
echo   Strict byte-for-byte comparison and reboot-hit are NOT proven here.
echo ------------------------------------------------------------------
echo.
echo log : C:\ProgramData\rb-deploy\l2-p0c-*.log
echo csv : C:\ProgramData\rb-deploy\l2-probe-*.csv
echo Both files share the same UTC timestamp for this run.
echo If the log is missing, confirm the folder C:\ProgramData\rb-deploy exists.
echo.
pause
exit /b %RC%
