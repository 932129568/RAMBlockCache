@echo off
REM diag-crash.cmd (2026-11-01) - self-elevating crash triage wrapper. ASCII only.
chcp 437 >nul
setlocal
cd /d "%~dp0"
if /i "%~1"=="elevated" goto work
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList 'elevated' -Verb RunAs"
exit /b
:work
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build\diag_crash.ps1"
echo.
echo ==== done - see build\crash-analyze.txt ====
timeout /t 5
exit /b
