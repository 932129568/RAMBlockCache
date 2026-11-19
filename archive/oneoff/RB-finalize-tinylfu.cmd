@echo off
rem ============================================================
rem  RB-finalize-tinylfu.cmd - F1 TinyLFU batch release (ONE UAC)
rem  Sign sys -> sync anchors -> deploy -> rebuild Setup -> sign Setup.
rem  Log: C:\ProgramData\rb-deploy\finalize-tinylfu.log
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_tinylfu.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_tinylfu.ps1"
exit /b %errorlevel%
