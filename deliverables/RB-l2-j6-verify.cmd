@echo off
rem ===========================================================================
rem  RB-l2-j6-verify.cmd
rem  J6 POST-REBOOT read-only verifier wrapper (user-approved 2026-11-13).
rem
rem  Double-click it ONCE after exactly ONE NORMAL Start-menu reboot.
rem  It asks for UAC and re-launches itself elevated, then runs:
rem    python src\scripts\j6_reboot_hit.py --manifest j6-manifest.json
rem  Output is echoed to the console AND logged under C:\ProgramData\rb-deploy\.
rem
rem  WHY ELEVATION IS MANDATORY: the verifier opens the RAMBlockCache control
rem  device via rbfeatctl stats and the volume \\.\D: with FILE_FLAG_NO_BUFFERING.
rem  Non-elevated rbfeatctl stats reports "open device failed"; volume reads
rem  fail with err=5. Those are reported as ENV (exit 2), never as a PASS.
rem
rem  READ-ONLY: no driver source/build/sign/deploy, no registry write, no
rem  configuration change, no write-back / WBCOAL (R4/R9 red lines), no
rem  method=2, no bootcfg POST, no verifier /reset, no rollback, no reboot.
rem
rem  THREE OUTCOMES (j6_reboot_hit.py, never relaxed):
rem    0 PASS          dCoreMiss at least 1, dL2Hits above 0, dL2Miss is 0
rem    5 COLD-START    l2state=1 but L2Hits=0 and L2Miss above 0, or the disk index
rem                   header Magic=0 / BootSeq mismatch. INCONCLUSIVE, NOT a
rem                   driver defect. RbL2IdxUnseal (rbL2.c:371) zeroes Magic
rem                   on open; only clean-shutdown H5 / RbL2IdxSave (rbL2.c:466)
rem                   finalises it. Forced power-off / hard reset leaves the
rem                   index unfinalised, so RbL2IdxLoad rejects it: cold start.
rem    5 CORE-INLINE   dCoreMiss=0: probe never ran, INCONCLUSIVE.
rem    1 FAIL          any other real mismatch.
rem    2 ENV           not elevated / artifacts missing / stats unreadable.
rem
rem  Exit codes: 0 PASS / 1 FAIL / 2 ENV / 5 INCONCLUSIVE or COLD-START.
rem  Log: C:\ProgramData\rb-deploy\j6-verify-YYYYMMDDTHHMMSSZ.log
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

set "MANIFEST=C:\ProgramData\rb-deploy\j6-manifest.json"
set "DEPLOY=C:\ProgramData\rb-deploy"
set "PY=%~dp0..\src\scripts\j6_reboot_hit.py"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting UAC ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

echo ==================================================================
echo   RAMBlockCache J6 REBOOT-HIT VERIFY - elevated, READ ONLY
echo   manifest: %MANIFEST%
echo ==================================================================
echo.
echo WRITE-BACK / WBCOAL stay OFF. No driver or config change here.
echo.

if not exist "%DEPLOY%" goto :no_dir
if not exist "%MANIFEST%" goto :no_manifest
if not exist "%PY%" goto :no_script
python --version >nul 2>&1
if not "%ERRORLEVEL%"=="0" goto :no_python

set "J6_PY=%PY%"
set "J6_MANIFEST=%MANIFEST%"
echo Running the verifier in the ELEVATED process; stdout + stderr go to a
echo UTC-stamped log under C:\ProgramData\rb-deploy\ ...
echo.
"%PWSH%" -NoProfile -ExecutionPolicy Bypass -Command "$log=Join-Path 'C:\ProgramData\rb-deploy' ('j6-verify-'+(Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')+'.log'); Write-Host ('log: '+$log); & python $env:J6_PY --manifest $env:J6_MANIFEST 2>&1 | Tee-Object -FilePath $log; $rc=$LASTEXITCODE; Write-Host ('VERIFIER EXIT = '+$rc); Write-Host ('log: '+$log); exit $rc"
set "VRC=%ERRORLEVEL%"
echo.
echo ==================================================================
echo   VERIFIER EXIT = %VRC%
echo ==================================================================
if "%VRC%"=="0" echo *** PASS - cross-boot L2 hit proven ***
if "%VRC%"=="1" echo *** FAIL - real defect, read the log ***
if "%VRC%"=="2" echo *** ENV / FIXTURE - elevated prompt or artifacts missing ***
if "%VRC%"=="5" echo *** INCONCLUSIVE or COLD-START - NOT a driver defect ***
echo.
echo COLD-START note: if the box was powered off forcibly / hard reset,
echo H5 did not finalise the L2 index. Do ONE NORMAL Start-menu reboot and
echo run RB-l2-j6-verify.cmd again. This is designed fail-closed behaviour.
echo Do NOT force power off and do NOT long-press the power button.
echo.
echo log: C:\ProgramData\rb-deploy\j6-verify-YYYYMMDDTHHMMSSZ.log
echo   (exact path printed above by the elevated PowerShell step)
echo manifest: %MANIFEST%
echo Cleanup AFTER J6 is finished: RB-l2-p0c-rollback.cmd
echo.
pause
exit /b %VRC%

:no_dir
echo *** ERROR: %DEPLOY% does not exist. Run RB-l2-j6-prepare.cmd first. ***
pause
exit /b 2

:no_manifest
echo *** ERROR: %MANIFEST% missing. Run RB-l2-j6-prepare.cmd first. ***
pause
exit /b 2

:no_script
echo *** ERROR: %PY% missing - repository layout changed. ***
pause
exit /b 2

:no_python
echo *** ERROR: python is not on PATH. Install it or use an elevated prompt. ***
pause
exit /b 2
