@echo off
rem ===========================================================================
rem  RB-finalize-172.cmd
rem  One elevated run: verify the built driver matches the reviewed hash, then
rem  sign -> sync anchors -> deploy to System32.  Does NOT reboot.
rem
rem  Usage: right-click -> "Run as administrator"  (or run from elevated cmd)
rem  Log   : %TEMP%\rb-sign-deploy-150.log
rem  Result: %TEMP%\rb-sign-deploy-150.result
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED ***
    echo Right-click this file and choose "Run as administrator".
    echo.
    pause
    exit /b 5
)

echo ==================================================================
echo   RAMBlockCache 172: sign + anchor sync + deploy   (NO REBOOT)
echo ==================================================================
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-finalize-172.ps1"

set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
    echo *** FAILED ^(exit=%RC%^) - nothing was deployed ***
) else (
    echo *** OK - deployed. REBOOT REQUIRED to load the new driver. ***
)
echo.
pause
exit /b %RC%
