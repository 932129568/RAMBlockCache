@echo off
chcp 65001 >nul
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "try { Start-Process -FilePath '%~f0' -Verb RunAs } catch { Write-Host ('ELEVATE FAIL: ' + $_.Exception.Message); pause }"
  exit /b
)
cd /d "%~dp0"
REM [FIX S-12] REM comments English-ized (batch encoding discipline).
REM [P1-7] Entry fix: the old chain install.cmd -> install_auto2.ps1 exited 1
REM unconditionally (install_auto2.ps1 is a deliberately retired placeholder
REM script), so a double-click only produced an error window.
REM Now points at scripts\install.ps1 (the authoritative install entry with the
REM same hash-gate model as deploy.ps1: compile -> sign -> register service ->
REM Volume UpperFilters -> re-enumerate).
echo [elevated] running install.ps1 ...
REM [P3] Dropped -NoExit: install.ps1 already ends with Read-Host to keep the
REM window open; -NoExit would only leave an extra empty powershell window
REM after the script finishes.
powershell -NoProfile -ExecutionPolicy Bypass -File "install.ps1"
