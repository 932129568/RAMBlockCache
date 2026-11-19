@echo off
rem ===========================================================================
rem  RB-l2-p0c-rollback.cmd
rem  Turn L2 off, clear L2Letter/L2SizeGB, delete the container and index on the
rem  container volume (C:), then re-run the read-only observation. Use this if the
rem  test was interrupted or if you left L2 on with -KeepOnPass.
rem
rem  Double-click it. It asks for UAC and re-launches itself elevated.
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting UAC ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

echo ==================================================================
echo   RAMBlockCache P0-C: ROLLBACK (L2 off, container deleted)
echo ==================================================================
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-p0c-test.ps1" -RollbackOnly

set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" echo *** rollback complete - L2 off, L2Letter/L2SizeGB clean ***
if not "%RC%"=="0" echo *** rollback INCOMPLETE (exit=%RC%) - read the log ***
echo.
pause
exit /b %RC%
