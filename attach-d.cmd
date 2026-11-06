@echo off
REM attach-d.cmd (2026-11-01) - manually attach D: with direct-write mode.
REM D: hosts the pagefile, so it is excluded from boot auto-attach by design;
REM manual attach has always been allowed. Self-elevates once (UAC).
chcp 437 >nul
setlocal
cd /d "%~dp0"
net session >nul 2>&1
if errorlevel 1 (
    echo [i] Administrator required - requesting UAC elevation...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" >nul 2>&1
    exit /b
)

set CTL=%~dp0src\ctl\ramboostctl.exe
if not exist "%CTL%" (
    echo [x] ramboostctl.exe not found: %CTL%
    pause
    exit /b 1
)

echo === attach D: (read=1 write=1 wm=0 = direct write) ===
"%CTL%" attach D read=1 write=1 wm=0
echo.
echo === volume list ===
"%CTL%" volumes
echo.
echo === stats ===
"%CTL%" status
echo.
pause
