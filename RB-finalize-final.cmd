@echo off
rem ============================================================
rem  RB-finalize-final.cmd - ONE UAC to finish the harvest cycle
rem  No rebuild: sign Setup -> gate5 -> verifier /reset.
rem  Then REBOOT ONCE: Verifier off + D: auto-resume + fixed bridge.
rem  Log: C:\ProgramData\rb-deploy\finalize-final.log
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_final.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_final.ps1"
exit /b %errorlevel%
