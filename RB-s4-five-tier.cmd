@echo off
rem RB-s4-five-tier.cmd - ONE UAC for the S4 geometry matrix (read-only on config; D: work/canary files).
rem Log: C:\ProgramData\rb-deploy\s4-five-tier.log
if not exist "C:\ProgramData\rb-deploy\94-SPEC-OK-LIFTED.ok" (
  echo [X] lift file missing.
  exit /b 1
)
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_s4_five_tier.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_s4_five_tier.ps1"
exit /b %errorlevel%
