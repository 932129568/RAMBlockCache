@echo off
REM =====================================================================
REM [ARCHIVE - Build 16] HISTORICAL COPY - DO NOT USE.
REM This file is byte-identical to the current src/scripts\offline_rollback.cmd
REM (V10.2 WinRE worst-case fallback); it is kept only as part of the Build 16
REM rollback-reference snapshot: read-only, do not delete (refactor spec E.6 / P1-5).
REM Use the copy under src/scripts\.
REM This header is the ONLY change made to this snapshot file.
REM =====================================================================
REM ============================================================
REM  offline_rollback.cmd  (V10.2 worst-case fallback, pure ASCII)
REM
REM  Use this ONLY if the machine will not boot after deploying the
REM  boot-start volume filter. It restores the Volume class
REM  UpperFilters to volsnap-only (removes RAMBlockCache) on the
REM  OFFLINE Windows registry hive, and clears the armed flag.
REM
REM  Usage (boot into WinRE):
REM    1. Boot from Windows install media / enter WinRE
REM       (Troubleshoot -> Advanced -> Command Prompt).
REM    2. Find the Windows drive (often not C: in WinRE).
REM       If it is NOT C:, edit the SYSROOT line below to match,
REM       e.g.  set SYSROOT=D:\Windows
REM    3. Run this script, e.g.:
REM          X:\path\to\offline_rollback.cmd
REM    4. Reboot. No network required.
REM ============================================================

setlocal EnableExtensions

set "SYSROOT=C:\Windows"
set "HIVE=%SYSROOT%\System32\config\SYSTEM"

if not exist "%HIVE%" (
  echo [X] Offline SYSTEM hive not found: %HIVE%
  echo     Edit this script and set SYSROOT= to your Windows drive
  echo     (try D:, E: ...). Then re-run.
  exit /b 1
)

echo [..] Loading offline SYSTEM hive ...
reg load HKLM\RB_OFFLINE "%HIVE%" >nul
if errorlevel 1 (
  echo [X] reg load failed. Is the hive already loaded?
  echo     Try: reg unload HKLM\RB_OFFLINE   then re-run.
  exit /b 1
)

set "CLS=SYSTEM\ControlSet001\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}"

echo [..] Removing RAMBlockCache from Volume UpperFilters (keep volsnap) ...
reg query "HKLM\RB_OFFLINE\%CLS%" /v UpperFilters >nul 2>&1
if not errorlevel 1 (
  reg delete "HKLM\RB_OFFLINE\%CLS%" /v UpperFilters /f >nul
)
REM Recreate UpperFilters = volsnap only (default Windows value).
REM If you had other custom volume filters, re-add them manually after reboot.
reg add "HKLM\RB_OFFLINE\%CLS%" /v UpperFilters /t REG_MULTI_SZ /d "volsnap\0" /f >nul

echo [..] Clearing armed flag in service Parameters ...
reg add "HKLM\RB_OFFLINE\ControlSet001\Services\RAMBlockCache\Parameters" /v FilterArmed /t REG_DWORD /d 0 /f >nul 2>&1

echo [..] Unloading hive ...
reg unload HKLM\RB_OFFLINE >nul

echo [OK] Rolled back. You can now reboot into Windows.
endlocal
