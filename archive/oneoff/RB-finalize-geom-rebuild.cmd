@echo off
rem ============================================================
rem  RB-finalize-geom-rebuild.cmd - ONE UAC for S2 full replay
rem  2026-11-07 geometry-rebuild release.
rem  Rebuild sys -> sign -> sync anchors -> gate5 -> rebuild Setup ->
rem  sign Setup -> verifier /reset -> deploy -> reboot marker.
rem  Log: C:\ProgramData\rb-deploy\finalize-geom-rebuild.log
rem  DO NOT RUN AFTER REBOOT: it would overwrite finalize-utc.txt.
rem  S4/S5 geometry switching needs NO further UAC or reboot.
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_geom_rebuild.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_geom_rebuild.ps1"
exit /b %errorlevel%
