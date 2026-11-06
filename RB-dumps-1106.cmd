@echo off
rem ============================================================
rem  RB-dumps-1106.cmd - stage crash dumps for U-8 analysis
rem  One UAC prompt. Copies all *.dmp to C:\Users\Public\rbc-dump-analysis
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\copy_dumps.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\copy_dumps.ps1"
