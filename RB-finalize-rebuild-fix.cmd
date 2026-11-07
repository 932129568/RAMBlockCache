@echo off
rem [93 SPEC NO-GO] FROZEN - DO NOT DEPLOY (31cf870 / 76aacfc / 931ed53)
echo [X] FROZEN: 93 SPEC NO-GO - DO NOT DEPLOY (31cf870 / 76aacfc / 931ed53)
echo     Reason: fail-open UAF / silent data corruption (Refs-baseline drain).
echo     See   : deliverables/93-spec-nogo-freeze-2026-11-07.md
echo     No signing, no deploy.ps1, no reboot. This chain must NOT run.
echo     Manual lift only, see src\scripts\finalize_rebuild_fix.ps1.
exit /b 1
rem ============================================================
rem  RB-finalize-rebuild-fix.cmd - ONE UAC for the 92 A/B/C fix
rem  DO NOT DEPLOY: 31cf870 / 76aacfc / 931ed53 are AUDIT RECORDS ONLY.
rem  2026-11-07. Re-signs the rebuilt driver, syncs the three sys
rem  hash anchors, repacks + re-signs the Setup, then DEPLOYS the new
rem  signed sys to System32\drivers in one narrow chain (92 3.1).
rem  Does NOT write the reboot marker, does NOT reset any test flag,
rem  does NOT reboot. An INTERACTIVE LOGON after reboot is REQUIRED.
rem  Log: C:\ProgramData\rb-deploy\finalize-rebuild-fix.log
rem  AFTER UAC: lead runs sync_setup_hash.ps1 -RequireSetupChanged
rem             then the user reboots ONCE and LOGS ON interactively
rem             (logon_disarm must clear FilterArmed).
rem  If System32 was locked, a queued replace may need a SECOND reboot.
rem  DO NOT re-run any full-chain finalize wrapper afterwards.
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_rebuild_fix.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_rebuild_fix.ps1"
exit /b %errorlevel%
