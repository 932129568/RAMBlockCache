@echo off
rem ===========================================================================
rem  RB-l2-j6-prepare.cmd
rem  J6 PREPARE (user-approved 2026-11-13): run the fixture ONCE with
rem  -KeepOnPass so L2 stays ON, then write a manifest and ask for ONE reboot.
rem
rem  Double-click it ONCE. It asks for UAC and re-launches itself elevated.
rem
rem  Runs: RB-l2-p0c-test.ps1 -ProbeStats -PostC -P5B -SliceTest
rem                            -WindowMin 3 -KeepOnPass
rem  -KeepOnPass SKIPS Invoke-Rollback on VERDICT: FIXTURE-PASS. That keeps
rem  the Features\Flags L2 bit, L2Letter/L2SizeGB, C:\RAMBoostL2c.bin,
rem  C:\RAMBoostL2c.idx and D:\rbc-l2-load.bin -- exactly the J6 precondition.
rem  This wrapper NEVER rolls back. Cleanup LATER: RB-l2-p0c-rollback.cmd
rem
rem  DRIVER-SAFETY: no source change, no build target, no sign, no deploy, no
rem  registry edit by this wrapper, no write-back / WBCOAL (R4/R9 red lines
rem  still in force), no method=2, no bootcfg POST, no verifier /reset, and
rem  this wrapper does NOT reboot the machine. The fixture enables L2 on ONE
rem  volume and leaves it ON; that is the whole point of the J6 preparation.
rem
rem  CROSS-BOOT INDEX-FINALISATION GATE (src/driver/rbL2.c, 2026-11-13):
rem    RbL2IdxUnseal (rbL2.c:371) writes Magic=0 to the disk index header as
rem    soon as the container is OPENED. -KeepOnPass only skips Invoke-Rollback;
rem    it does NOT call RbL2IdxSave (rbL2.c:466). Only a CLEAN SHUTDOWN H5
rem    power path or RbL2CloseIntEx(Persist=TRUE) finalises the index.
rem    Therefore the user MUST reboot via START MENU then RESTART. Forced
rem    power-off, long-press power button, hard reset or abnormal shutdown
rem    leaves Magic=0, so RbL2IdxLoad (rbL2.c:315-360) rejects the whole
rem    index and the boot is a COLD START with L2Hits=0 and L2Miss above 0.
rem    That is designed fail-closed behaviour, NOT a driver defect.
rem    driver defect. j6_reboot_hit.py reports it as INCONCLUSIVE (exit 5),
rem    never FAIL. The disk index header is snapshotted into j6-manifest.json
rem    via src\scripts\parse_l2_idx.py (read-only) for post-reboot comparison.
rem
rem  Manifest: C:\ProgramData\rb-deploy\j6-manifest.json
rem    baseline_boot_utc / baseline_boot_unix = boot time at prepare
rem    blocks_json = the p5b-blocks-STAMP.json made by THIS fixture run
rem    expect_sha  = sha256 of C:\Windows\System32\drivers\RAMBlockCache.sys
rem    index_prepare = disk index Magic / BootSeq / Count + parse_l2_idx.py output
rem  Verify entry: deliverables\RB-l2-j6-verify.cmd (self-elevates; logs to
rem    C:\ProgramData\rb-deploy\), which calls j6_reboot_hit.py --manifest
rem
rem  Exit codes: 0 = prepared / 1 = fixture did not PASS or manifest refused.
rem  Expected runtime: about 4 to 5 minutes. Keep the window open.
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

rem Capture the UTC start instant so the manifest can PROVE the p5b report is
rem from THIS run (not a stale p5b-blocks-*.json left over from 11-12/11-13).
for /f "delims=" %%A in ('powershell -NoProfile -Command "[math]::Round(((Get-Date).ToUniversalTime()-[datetime]::new(1970,1,1,0,0,0,[DateTimeKind]::Utc)).TotalSeconds,0)"') do set "J6_START_EPOCH=%%A"
if "%J6_START_EPOCH%"=="" goto :no_epoch

echo ==================================================================
echo   RAMBlockCache J6 PREPARE - elevated
echo   container C: 2 GB   load D:   window 3 min   L2 stays ON
echo   switches: -ProbeStats -PostC -P5B -SliceTest -WindowMin 3 -KeepOnPass
echo ==================================================================
echo.
echo WRITE-BACK / WBCOAL stay OFF. No driver build / sign / deploy here.
echo Expected runtime: about 4 to 5 minutes. Do NOT close this window.
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-p0c-test.ps1" -ProbeStats -PostC -P5B -SliceTest -WindowMin 3 -KeepOnPass
set "RC=%ERRORLEVEL%"

echo.
echo ==================================================================
echo   Fixture exit code: %RC%
echo ==================================================================
if not "%RC%"=="0" goto :fixture_not_pass

rem -KeepOnPass must have left all three artifacts behind. Fail closed if not.
if not exist "C:\RAMBoostL2c.bin" goto :artifacts_missing
if not exist "C:\RAMBoostL2c.idx" goto :artifacts_missing
if not exist "D:\rbc-l2-load.bin" goto :artifacts_missing

rem Write the J6 manifest. Pure ASCII source; CRLF enforced by the wrapper.
rem [FIX G2 2026-11-15] This writer used to be an inline -Command that died on
rem hosts where Get-FileHash does not resolve (report 244 / G1, same defect
rem class), AFTER the fixture had already left L2 ON. The logic now lives in
rem src\scripts\j6_write_manifest.ps1: .NET SHA256 over a FileShare.ReadWrite
rem handle, no cmdlet dependency, and the same fail-closed freshness check
rem (the fixture log must be newer than the J6_START_EPOCH captured above).
"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\src\scripts\j6_write_manifest.ps1" -FixtureExit %RC% -StartUnix %J6_START_EPOCH%
set "MRC=%ERRORLEVEL%"
if not "%MRC%"=="0" goto :manifest_failed

echo.
echo ==================================================================
echo   REBOOT ONCE from the Start menu (normal restart).
echo   DO NOT force power off / hard reset / long-press power.
echo   Reason: only a clean shutdown runs the H5 IdxSave path that finalises
echo   the L2 index; a hard reset leaves Magic=0 -^> cold start -^>
echo   INCONCLUSIVE (not a driver defect). -KeepOnPass does NOT call IdxSave.
echo   After the reboot, double-click RB-l2-j6-verify.cmd (self-elevates).
echo ==================================================================
python "%~dp0..\src\scripts\j6_reboot_hit.py" --notice
if not "%ERRORLEVEL%"=="0" echo   [WARN] python not found; use the ASCII notice above.
echo.
echo   RB-l2-j6-verify.cmd self-elevates, reads j6-manifest.json, runs
echo   j6_reboot_hit.py --manifest and writes a UTC-stamped log under
echo   C:\ProgramData\rb-deploy\. Do NOT run the .py directly: it needs an
echo   elevated process for rbfeatctl stats and the D: volume read.
echo   Verifier exit: 0=PASS / 1=FAIL / 2=ENV / 5=INCONCLUSIVE or COLD-START.
echo   Cleanup AFTER J6 is finished: RB-l2-p0c-rollback.cmd
echo.
pause
exit /b 0

:no_epoch
echo *** ERROR: could not capture the UTC start instant. Nothing prepared. ***
pause
exit /b 1

:fixture_not_pass
echo *** ERROR: fixture exit=%RC% is not 0 (FIXTURE-PASS + -KeepOnPass). ***
echo *** L2 may already have been rolled back. J6 is NOT prepared.     ***
echo *** Read C:\ProgramData\rb-deploy\l2-p0c-*.log before retrying.     ***
pause
exit /b 1

:artifacts_missing
echo *** ERROR: -KeepOnPass artifacts missing (container / index / load). ***
echo *** J6 is NOT prepared. Check the fixture log; do NOT reboot yet.  ***
pause
exit /b 1

:manifest_failed
echo *** ERROR: manifest write refused (exit=%MRC%). J6 is NOT prepared.  ***
echo *** Nothing was rolled back; L2 is still ON. Inspect the log above.  ***
pause
exit /b 1
