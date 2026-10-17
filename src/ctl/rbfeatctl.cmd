@echo off
rem rbfeatctl.cmd - convenience wrapper for rbfeatctl.exe (feature layer: stats / vols / L2)
rem Same rationale as ramboostctl.cmd: exe beside this file + one-time UAC self-elevation
rem (control device SDDL is admins-only, 0x5 otherwise).
rem Usage: rbfeatctl.cmd stats | vols | refresh | l2 ...
rem Args with embedded spaces are NOT quoted through UAC relay; keep args simple.
setlocal
chcp 65001 >nul 2>&1
if "%RB_ELEVATED%"=="1" goto run
net session >nul 2>&1
if errorlevel 1 (
    echo [i] requesting administrator rights ^(control device is admins-only^) ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs -WorkingDirectory '%~dp0'"
    if errorlevel 1 echo [!] UAC declined or elevation failed - run this .cmd as Administrator manually.
    exit /b
)
:run
if not exist "%~dp0rbfeatctl.exe" (
    echo [!] rbfeatctl.exe not found next to this .cmd.
    echo     expected dir: %~dp0
    pause
    exit /b 2
)
"%~dp0rbfeatctl.exe" %*
echo.
pause
endlocal
