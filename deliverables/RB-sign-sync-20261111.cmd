# RB-sign-sync-20261111.cmd
# One-shot: sign the freshly built driver, install it into dist\, re-sync all
# three hash anchors, and verify the whole chain. Runs elevated once.
#
# Built artifact : src\driver\x64\Release\RAMBlockCache.sys
#                  sha256=3c5b32a46f9bce58814ef9d8fb7cc01979589ec65355e09fdd878f764d730a0f
#                  size=184320
# Cert           : CN=RAMBoostFS Root / thumb DF1A0ADA...431
#
# Deliberately does NOT touch System32 or reboot -- deployment is a separate,
# explicit step.

@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
set "LOG=%TEMP%\rb-sign-sync-20261111.log"

echo ============================================================
echo  RAMBlockCache signing + anchor sync  (no deploy, no reboot)
echo ============================================================
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-sign-sync-20261111.ps1" 2>&1 | Tee-Object -FilePath "%LOG%"

set "RC=%ERRORLEVEL%"
echo.
echo Log: %LOG%
if not "%RC%"=="0" (
    echo.
    echo *** FAILED (exit=%RC%) ***
) else (
    echo.
    echo *** OK ***
)
echo.
pause
exit /b %RC%
