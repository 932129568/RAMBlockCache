@echo off
rem RB-sign-deploy-147.cmd
rem One elevated run: sign the freshly built driver -> dist\ -> anchors ->
rem verify -> deploy to System32 -> post-verify (hash/signature/registry/DeployAck).
rem Does NOT reboot. Reboot is a separate explicit decision.
rem
rem Usage: right-click -> "Run as administrator"  (or run from an elevated cmd)
rem Result: %TEMP%\rb-sign-deploy-147.result   Log: %TEMP%\rb-sign-deploy-147.log

setlocal EnableDelayedExpansion
chcp 65001 >nul

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
set "LOG=%TEMP%\rb-sign-deploy-147.log"
set "RES=%TEMP%\rb-sign-deploy-147.result"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED ***
    echo Right-click this file and choose "Run as administrator".
    echo.
    pause
    exit /b 5
)

echo ============================================================
echo  RAMBlockCache 147: sign + anchor sync + deploy (no reboot)
echo ============================================================
echo.

if exist "%RES%" del /q "%RES%"

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-sign-deploy-147.ps1"

set "RC=%ERRORLEVEL%"
echo.
echo Log    : %LOG%
echo Result : %RES%
if not "%RC%"=="0" (
    echo.
    echo *** FAILED ^(exit=%RC%^) ***
) else (
    echo.
    echo *** OK - deployed, NOT rebooted ***
)
echo.
pause
exit /b %RC%
