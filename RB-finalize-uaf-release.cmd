@echo off
rem ============================================================
rem  RB-finalize-uaf-release.cmd - ONE UAC for >4K UAF fix full replay
rem  Rebuild sys -> sign -> sync anchors -> gate5 -> rebuild Setup ->
rem  sign Setup -> verifier /reset -> deploy -> reboot marker.
rem  DO NOT RUN AFTER REBOOT: it would overwrite finalize-utc.txt.
rem  Log: C:\ProgramData\rb-deploy\finalize-uaf-release.log
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_uaf_release.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_uaf_release.ps1"
exit /b %errorlevel%
