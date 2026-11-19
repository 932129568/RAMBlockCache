@echo off
rem ===========================================================================
rem  RB-l2-c-p5.cmd
rem  C-plan P5 acceptance: enable L2 on ONE volume, observe, auto-rollback.
rem
rem  Double-click it ONCE. It asks for UAC and re-launches itself elevated.
rem  Runs RB-l2-p0c-test.ps1 with -ProbeStats -PostC -P5B.
rem
rem  P5-B decisive re-read experiment (round 2): reads live container
rem  record headers, re-reads the load file in 64KB aligned chunks, then
rem  issues one 64KB unbuffered volume read per stored block LBA.
rem  All read-only. No driver change, no re-sign, no reboot.
rem
rem  PRECONDITION: P4 deploy + ONE reboot already done. The kernel MUST be
rem  running the C-plan signed driver 37700a686... - Authenticode Valid.
rem  This wrapper does NOT verify the reboot - confirm it yourself.
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
echo   RAMBlockCache C-PLAN P5 ACCEPTANCE  - elevated
echo   container C:  2 GB    load D:    window 10 min
echo   switches: -ProbeStats -PostC -P5B
echo ==================================================================
echo.
echo PRECONDITION: P4 deploy done AND the machine rebooted ONCE already.
echo Kernel must be the C-plan signed driver 37700a686... - Authenticode Valid.
echo This wrapper cannot prove the reboot - confirm it before continuing.
echo Expected runtime: about 11 to 12 minutes. Do NOT close this window.
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-p0c-test.ps1" -ProbeStats -PostC -P5B

set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" echo *** PASS - L2 executed, box clean, rolled back to known state ***
if "%RC%"=="1" echo *** FAIL - rolled back, read the log ***
if "%RC%"=="2" echo *** REFUSED at preflight - nothing was changed ***
if "%RC%"=="3" echo *** RECOVERED a crashed run - rolled back, run this again ***
if "%RC%"=="4" echo *** INTERNAL ERROR - nothing reliable happened ***
if "%RC%"=="5" echo *** UNCOVERED - L2 opened, no store executed, stability only, NOT a caching PASS ***
echo.
echo ------------------------------------------------------------------
echo IMPORTANT - verdict boundary, 203 criterion 4:
echo   Any LBA OBSERVATION line in the log is STATIC geometry only.
echo   It does NOT prove out-of-window blocks were served, and it is
echo   NOT a PASS of the out-of-window special test.
echo   Real evidence is: l2s greater than 0 AND l2h greater than 0,
echo   together with blk_min greater than s_Slots, seen in the final
echo   checks and the probe CSV. Exit 5 UNCOVERED is NOT a PASS.
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
