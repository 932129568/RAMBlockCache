@echo off
rem ============================================================
rem  RB-finalize-1106.cmd - one-click elevated release finalizer
rem  Signs the freshly built Setup + arms local guard tasks +
rem  refreshes rb-deploy staging. Exactly ONE UAC prompt.
rem  Log: C:\ProgramData\rb-deploy\finalize-1106.log
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_release.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_release.ps1"
exit /b %errorlevel%
