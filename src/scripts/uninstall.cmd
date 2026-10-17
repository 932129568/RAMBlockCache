@echo off
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0uninstall.ps1\"'"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
