@echo off
setlocal
rem RAMBlockCache L2 read-only observe (V1..V6). No writes, no enable/disable.
cd /d "%~dp0"

pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-observe.ps1" %*
set "RC=%ERRORLEVEL"

echo.
echo [RB-l2-observe] exit code = %RC   (0=all PASS, 1=has FAIL, 2=no FAIL but UNKNOWN)
pause
endlocal & exit /b %RC
