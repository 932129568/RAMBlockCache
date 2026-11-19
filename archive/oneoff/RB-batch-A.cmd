@echo off
rem ============================================================
rem  RB-batch-A.cmd - round A: sign + stage + deploy new driver
rem  One UAC prompt. Log: C:\ProgramData\rb-deploy\finalize-batch.log
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_batch.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_batch.ps1"
