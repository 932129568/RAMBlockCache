@echo off
rem ============================================================
rem  RB-restart-bridge.cmd - ONE UAC, reload the Web bridge only.
rem  Loads the TinyLFU config-chain fix (decodeConfig / encodeParams)
rem  into the live SYSTEM node process via the rb-bridge keeper.
rem  No driver config change, no reboot, no Verifier reset.
rem  Log: C:\ProgramData\rb-deploy\restart-bridge.log
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\restart_bridge.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\restart_bridge.ps1"
exit /b %errorlevel%
