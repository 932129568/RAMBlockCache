@echo off
rem ============================================================
rem  RB-finalize-u6.cmd - U-6 closure (ONE UAC prompt)
rem  Signs the silent engine, rebuilds Setup, re-signs Setup.
rem  Log: C:\ProgramData\rb-deploy\finalize-u6.log
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_u6.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_u6.ps1"
exit /b %errorlevel%
