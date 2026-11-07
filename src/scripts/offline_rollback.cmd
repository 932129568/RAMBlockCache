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
REM    2. The Windows drive is auto-detected below (C:..Z:).
REM       WinRE drive letters often differ from the booted system;
REM       no manual SYSROOT edit is needed.
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
REM [FIX R6-S1 2026-10-21] Detection MUST run AFTER reg load: the query reads
REM HKLM\RB_OFFLINE\Select, which only exists once the offline hive is mounted.
REM Original order queried before loading -> in a clean WinRE session _CSN is
REM always empty -> unconditional exit 1 (the only 0x7B rescue script was 100%
REM dead in a clean session). Build16 archive confirms load-then-detect order.

REM [88 H-4 2026-11-07] Auto-detect the Windows installation drive instead of
REM hardcoding C:\Windows (WinRE commonly assigns D:, E:, ...).
set "SYSROOT="
for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
  if not defined SYSROOT if exist "%%D:\Windows\System32\config\SYSTEM" set "SYSROOT=%%D:\Windows"
)
if not defined SYSROOT (
  echo [X] Could not locate a Windows installation (C:..Z:\Windows\System32\config\SYSTEM).
  echo     Use diskpart to find the Windows volume, then re-run. No changes made.
  exit /b 1
)
set "HIVE=%SYSROOT%\System32\config\SYSTEM"
if not exist "%HIVE%" (
  echo [X] Offline SYSTEM hive not found: %HIVE%
  exit /b 1
)
echo [..] Detected Windows: %SYSROOT%

echo [..] Loading offline SYSTEM hive ...
reg load HKLM\RB_OFFLINE "%HIVE%" >nul
if errorlevel 1 (
  echo [X] reg load failed. Is the hive already loaded?
  echo     Try: reg unload HKLM\RB_OFFLINE   then re-run.
  exit /b 1
)

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
REM [88 H-4 2026-11-07] Pure reg.exe read-modify-write, no PowerShell dependency
REM (stock WinRE often has no powershell.exe). reg query renders REG_MULTI_SZ
REM as one line whose entries are joined by literal \0; convert to ';', drop
REM only RAMBlockCache, keep every other filter, fall back to volsnap when
REM empty. reg add with /t REG_MULTI_SZ and \0 separators writes it back.
set "UF_RAW="
set "UF_TYPE="
for /f "tokens=2,*" %%A in ('reg query "HKLM\RB_OFFLINE\%CLS%" /v UpperFilters 2^>nul ^| findstr /i /c:"UpperFilters"') do (
  set "UF_TYPE=%%A"
  set "UF_RAW=%%B"
)
if not defined UF_RAW goto UF_ABSENT
if /i not "%UF_TYPE%"=="REG_MULTI_SZ" (
  echo [X] UpperFilters is not REG_MULTI_SZ (got %UF_TYPE%). Refusing to rewrite.
  reg unload HKLM\RB_OFFLINE >nul 2>&1
  exit /b 1
)
set "UF_LIST=!UF_RAW:\0=;!"
set "UF_NEW="
set "UF_REST=!UF_LIST!"
:UF_LOOP
if not defined UF_REST goto UF_WRITE
set "UF_E="
for /f "tokens=1* delims=;" %%A in ("!UF_REST!") do (
  set "UF_E=%%A"
  set "UF_REST=%%B"
)
if defined UF_E if /i not "!UF_E!"=="RAMBlockCache" (
  if defined UF_NEW (set "UF_NEW=!UF_NEW!\0!UF_E!") else (set "UF_NEW=!UF_E!")
)
goto UF_LOOP
:UF_WRITE
if not defined UF_NEW set "UF_NEW=volsnap"
reg add "HKLM\RB_OFFLINE\%CLS%" /v UpperFilters /t REG_MULTI_SZ /d "!UF_NEW!" /f >nul 2>&1
if errorlevel 1 (
  echo [X] UpperFilters strip FAILED - the offline hive was NOT modified.
  echo     Do NOT reboot believing this is rolled back. Inspect with:
  echo       reg query "HKLM\RB_OFFLINE\%CLS%" /v UpperFilters
  reg unload HKLM\RB_OFFLINE >nul 2>&1
  exit /b 1
)
echo [ok] UpperFilters rewritten without RAMBlockCache: !UF_NEW!
goto UF_DONE
:UF_ABSENT
echo [..] UpperFilters not present, nothing to strip
:UF_DONE

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
