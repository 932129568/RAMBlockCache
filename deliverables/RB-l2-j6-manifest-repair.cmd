@echo off
rem ===========================================================================
rem  RB-l2-j6-manifest-repair.cmd
rem  Recovery for the exact state produced by the G2 defect (2026-11-15):
rem
rem    Fixture exit code: 0
rem    Get-FileHash : The term 'Get-FileHash' is not recognized ...
rem    *** ERROR: manifest write refused (exit=1). J6 is NOT prepared. ***
rem    *** Nothing was rolled back; L2 is still ON. ***
rem
rem  In that state the fixture has ALREADY done everything J6 needs:
rem  Features\Flags L2 bit ON, L2Letter/L2SizeGB, C:\RAMBoostL2c.bin,
rem  C:\RAMBoostL2c.idx, D:\rbc-l2-load.bin and the p5b-blocks-<stamp>.json.
rem  Only C:\ProgramData\rb-deploy\j6-manifest.json is missing.
rem
rem  This wrapper re-writes ONLY that manifest, from the newest fixture log and
rem  its stage-A report. It does NOT rerun the fixture, does NOT roll back,
rem  does NOT reboot, does NOT restart or reconfigure the driver, and does NOT
rem  touch Features\Flags. Idempotent: run it twice, same manifest content
rem  except created_utc.
rem
rem  Freshness is still enforced (fail-closed): the fixture log must be newer
rem  than the current boot time, so a leftover report from an older boot is
rem  refused instead of silently becoming this run's baseline.
rem
rem  Double-click it once. It asks for UAC and re-launches itself elevated,
rem  because it writes under C:\ProgramData.
rem
rem  Exit codes: 0 = manifest written / 2 = refused (nothing written).
rem  After this succeeds: Start menu -> Restart (NORMAL shutdown, NOT a forced
rem  power-off), then double-click RB-l2-j6-verify.cmd.
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
echo   RAMBlockCache J6 MANIFEST REPAIR - elevated, read-only otherwise
echo   no fixture re-run / no rollback / no reboot / no driver change
echo ==================================================================
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\src\scripts\j6_write_manifest.ps1" -FixtureExit 0
set "RC=%ERRORLEVEL%"

echo.
echo ==================================================================
echo   manifest writer exit code: %RC%
echo ==================================================================
if not "%RC%"=="0" goto :refused

echo.
echo NEXT: Start menu -^> Restart (normal restart).
echo DO NOT force power off / hard reset / long-press power: only a clean
echo shutdown runs the H5 IdxSave path that finalises the L2 index.
echo Then double-click RB-l2-j6-verify.cmd.
echo.
pause
exit /b 0

:refused
echo *** REFUSED: no manifest written. Do NOT reboot yet.       ***
echo *** Read the [X] line above. Common causes:                ***
echo ***   - newest l2-p0c-*.log predates this boot (stale run) ***
echo ***   - p5b-blocks-^<stamp^>.json missing for that log      ***
echo ***   - deployed image missing / unreadable               ***
echo *** If the fixture never PASSed, run RB-l2-j6-prepare.cmd  ***
echo *** instead (it re-runs the fixture with -KeepOnPass).     ***
echo.
pause
exit /b 2
