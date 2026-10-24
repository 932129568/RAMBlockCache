@echo off
REM =====================================================================
REM [ARCHIVE - Build 16] HISTORICAL COPY - DO NOT USE.
REM This snapshot is kept only as a ROLLBACK REFERENCE: read-only, do not
REM delete (refactor spec E.6 / P1-5).
REM It drives the old Build 16 deploy.ps1 (no fail-closed gate - see the
REM header of that file). Current install entry: setup\ramboostsetup.ps1
REM (or RamBoostSetup.exe); current deploy: scripts\deploy.cmd.
REM This header is the ONLY change made to this snapshot file.
REM =====================================================================
echo === RAMBlockCache Build 16 Installer ===
echo.
echo This will:
echo   1. Copy signed driver to System32\drivers
echo   2. Set service to BOOT_START
echo   3. Add RAMBlockCache to UpperFilters (volsnap + RAMBlockCache)
echo   4. Install boot-guard scheduled tasks
echo.
echo You MUST reboot after this completes.
echo.
pause
powershell -ExecutionPolicy Bypass -File "%~dp0deploy.ps1"
echo.
echo === Done. Please reboot now. ===
pause