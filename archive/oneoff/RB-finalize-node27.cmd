@echo off
rem ============================================================
rem  RB-finalize-node27.cmd - ONE UAC for the Node 27 switch
rem  Pin Machine/User PATH -> rebuild Setup (Node 27 + npm 12.2.1
rem  built in) -> sign Setup. Driver sys unchanged, no reboot.
rem  Log: C:\ProgramData\rb-deploy\finalize-node27.log
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_node27.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_node27.ps1"
exit /b %errorlevel%
