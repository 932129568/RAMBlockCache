@echo off
rem ============================================================
rem  RB-repack-setup.cmd - ONE UAC for a FRONTEND-ONLY Setup repackage
rem  2026-11-07. Rebuild + re-sign dist\RAMBlockCache-Setup.exe ONLY.
rem  The Web console BlockSize dropdown now offers 4/8/16/32 KB; the
rem  distribution Setup still embeds the old single-size SPA.
rem  sys / three sys-hash anchors / reboot marker are NOT modified.
rem  No driver rebuild, no deployment, no reboot required.
rem  Log: C:\ProgramData\rb-deploy\finalize-setup-only.log
rem  AFTER UAC: lead runs sync_setup_hash.ps1 -RequireSetupChanged
rem  DO NOT re-run any full-chain finalize wrapper afterwards.
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_setup_only.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_setup_only.ps1"
exit /b %errorlevel%
