@echo off
REM verify-now.cmd (2026-11-01) - robust one-click elevated verification.
REM ASCII only. Logs every step to build\verify-now.log BEFORE doing work,
REM so an instant close still leaves evidence. No dependency on "net session"
REM after elevation (uses an argument marker instead of a loop-prone recheck).
chcp 65001 >nul
setlocal
cd /d "%~dp0"
set LOG=%~dp0build\verify-now.log
if not exist "%~dp0build" mkdir "%~dp0build"

echo [%DATE% %TIME%] wrapper start argv=[%*] >> "%LOG%"

if /i "%~1"=="elevated" goto work

echo [%DATE% %TIME%] not elevated - requesting UAC ... >> "%LOG%"
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList 'elevated' -Verb RunAs" >> "%LOG%" 2>&1
echo [%DATE% %TIME%] elevation spawn rc=%ERRORLEVEL% >> "%LOG%"
if errorlevel 1 (
    echo.
    echo [!] UAC elevation was refused or failed. Nothing was verified.
    echo     Re-run this file with right-click -^> Run as administrator.
    timeout /t 8
)
exit /b

:work
echo [%DATE% %TIME%] elevated instance - running verify_now.ps1 >> "%LOG%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build\verify_now.ps1" >> "%LOG%" 2>&1
set RC=%ERRORLEVEL%
echo [%DATE% %TIME%] verify_now.ps1 exit=%RC% >> "%LOG%"
echo.
echo ==== done (exit %RC%) - log: build\verify-now.log ====
timeout /t 5
exit /b
