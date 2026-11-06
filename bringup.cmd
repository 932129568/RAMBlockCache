@echo off
REM bringup.cmd (2026-11-01) - one-click elevated bringup.
REM Self-elevates (one UAC prompt), then runs build\elev_bringup.ps1:
REM   sign new driver -> sync 3 anchors -> check_signature gate ->
REM   remove RAMBlockCache from DiskDrive class -> deploy.ps1
REM   (System32 copy + BOOT_START + StorageVolume UpperFilters + arm + guard tasks + verify)
REM ASCII only on purpose (no codepage issues with the non-ASCII repo path).
chcp 437 >nul
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
    echo [i] Administrator required - requesting UAC elevation...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo === elevated - running bringup ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build\elev_bringup.ps1"
set RC=%ERRORLEVEL%
echo.
echo ---- exit code: %RC% ----
echo log: %~dp0build\elev-bringup.log
echo.
echo NEXT: reboot once, then run verify-live.cmd as administrator.
echo If the machine does not boot: run rollback-upperfilters.cmd from WinRE
echo   (or scripts\offline_rollback.cmd).
echo.
pause
