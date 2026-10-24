@echo off
rem ramboostctl.cmd - convenience wrapper for ramboostctl.exe
rem Fixes two recurring frictions:
rem   1. exe not on PATH  -> this .cmd calls the exe beside itself
rem   2. 0x5 ACCESS_DENIED -> control device SDDL is D:P(A;;GA;;;SY)(A;;GA;;;BA)
rem      (SYSTEM + built-in admins only), so we self-elevate once via UAC.
rem Usage: ramboostctl.cmd volumes | stats | get-config | set enabled=1 vol=D: ...
rem Args with embedded spaces are NOT quoted through UAC relay; keep args simple.
setlocal
chcp 65001 >nul 2>&1
rem [FIX S-12] Elevation gate is the net session check below; RB_ELEVATED was a dead
rem variable (UAC relay never sets it).
net session >nul 2>&1
if errorlevel 1 (
    echo [i] requesting administrator rights ^(control device is admins-only^) ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs -WorkingDirectory '%~dp0'"
    if errorlevel 1 echo [!] UAC declined or elevation failed - run this .cmd as Administrator manually.
    exit /b
)
if not exist "%~dp0ramboostctl.exe" (
    echo [!] ramboostctl.exe not found next to this .cmd.
    echo     expected dir: %~dp0
    pause
    exit /b 2
)
"%~dp0ramboostctl.exe" %*
echo.
pause
endlocal
