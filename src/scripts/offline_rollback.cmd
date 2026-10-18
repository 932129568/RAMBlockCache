@echo off
REM ============================================================
REM  offline_rollback.cmd  (V10.2 worst-case fallback, pure ASCII)
REM
REM  Use this ONLY if the machine will not boot after deploying the
REM  boot-start volume filter. It strips RAMBlockCache from the Volume
REM  class UpperFilters (keeping every other filter, incl. volsnap and
REM  any third-party filter) on the OFFLINE Windows registry hive, and
REM  clears the armed flag.
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

setlocal EnableExtensions EnableDelayedExpansion

REM [P1-9] Detect the ControlSet actually used by this boot: Select\Current
REM is a DWORD (0x1/0x2/0x3...) mapping to ControlSet001/002/003.
REM Legacy script hardcoded ControlSet001: if the machine last booted 002/003
REM (snapshot restore, dual boot, reinstall leftovers) the rewritten registry
REM is not the active set -> rollback no-ops and 0x7B continues.
set "CSD="
for /f "tokens=3" %%D in ('reg query "HKLM\RB_OFFLINE\Select" /v Current 2^>nul ^| findstr /i /c:"Current"') do set "_CSN=%%D"
if defined _CSN (
  if /i "%_CSN%"=="0x1" set "CSD=001"
  if /i "%_CSN%"=="0x2" set "CSD=002"
  if /i "%_CSN%"=="0x3" set "CSD=003"
)
REM [FIX sev-5 2026-10-18] No silent fallback to ControlSet001 on unmatched
REM mapping (writing the inactive set fakes success while 0x7B remains).
REM Print the raw value and exit for manual confirmation instead.
if not defined CSD (
  echo [X] Could not map Select\Current value "%_CSN%" to ControlSet001/002/003.
  echo     Refusing to guess: writing the wrong ControlSet would fake success
  echo     while the boot loop remains. Check with:
  echo       reg query "HKLM\RB_OFFLINE\Select" /v Current
  echo     then edit this script (the mapping block) and re-run.
  reg unload HKLM\RB_OFFLINE >nul 2>&1
  exit /b 1
)
set "CS=SYSTEM\ControlSet%CSD%"
echo [..] Target ControlSet: %CS%
set "CLS=%CS%\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}"

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

echo [..] Removing RAMBlockCache from Volume UpperFilters (keep other filters) ...
REM [P1-9] Strip only the RAMBlockCache entry; keep every other volume
REM filter (incl. third-party) untouched. Legacy script reset UpperFilters
REM to volsnap-only, silently deleting third-party filters - this is the
REM last-resort recovery for 0x7B and must not break other stack parts.
REM Empty result falls back to Windows default volsnap (missing value may
REM prevent volsnap from loading).
REM [FIX sev-5 2026-10-18] The strip step no longer fails silently
REM (no errorlevel check before: strip failure still printed [OK] later,
REM user reboots believing rollback happened while registry was untouched).
powershell -NoProfile -ExecutionPolicy Bypass -Command "$p='HKLM:\RB_OFFLINE\%CLS%'; $v=(Get-ItemProperty -Path $p -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters; if($null -ne $v){ $keep=@($v | Where-Object { $_ -ne 'RAMBlockCache' }); if($keep.Count -eq 0){ $keep=@('volsnap') }; New-ItemProperty -Path $p -Name UpperFilters -PropertyType MultiString -Value $keep -Force | Out-Null; Write-Host ('[ok] UpperFilters now: ' + ($keep -join '; ')); exit 0 } else { Write-Host '[..] UpperFilters not present, nothing to strip'; exit 0 }"
if errorlevel 1 (
  echo [X] UpperFilters strip FAILED - the offline hive was NOT modified.
  echo     Do NOT reboot believing this is rolled back. Inspect with:
  echo       reg query "HKLM\RB_OFFLINE\%CLS%" /v UpperFilters
  reg unload HKLM\RB_OFFLINE >nul 2>&1
  exit /b 1
)

echo [..] Clearing armed flag in service Parameters ...
reg add "HKLM\RB_OFFLINE\%CS%\Services\RAMBlockCache\Parameters" /v FilterArmed /t REG_DWORD /d 0 /f >nul 2>&1
if errorlevel 1 echo [!] Warning: clearing FilterArmed failed (non-fatal, upper filters already stripped)

echo [..] Unloading hive ...
reg unload HKLM\RB_OFFLINE >nul
REM [FIX sev-5 2026-10-18] reg unload failure = lazy-flushed hive: a forced
REM power-off may lose the rollback writes just made. Must be loud, not [OK].
if errorlevel 1 (
  echo [X] reg unload FAILED - changes may still be in a lazy-flushed hive.
  echo     Wait ~10 seconds and run "reg unload HKLM\RB_OFFLINE" again,
  echo     or run "reg query HKLM\RB_OFFLINE" to check. Do NOT force reboot
  echo     until unload succeeds, or the rollback may be lost.
  exit /b 1
)

echo [OK] Rolled back. You can now reboot into Windows.
endlocal
