@echo off
rem ============================================================
rem  RB-finalize-distribution-fix.cmd - ONE UAC for the FINAL rebuild
rem  2026-11-07. Rebuilds + re-signs the silent engine and the single
rem  file Setup so batches A and B reach the distribution package.
rem  sys / three sys-hash anchors / reboot marker are NOT modified.
rem  No driver rebuild, no driver signing, no deployment, no reboot.
rem  Log: C:\ProgramData\rb-deploy\finalize-distribution-fix.log
rem  AFTER UAC: lead runs sync_setup_hash.ps1 -RequireSetupChanged
rem  DO NOT re-run any full-chain finalize wrapper afterwards.
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_distribution_fix.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_distribution_fix.ps1"
exit /b %errorlevel%
