@echo off
setlocal
chcp 437 >nul
rem ===================================================================
rem  RB-glass-probe.cmd  [293 2026-11-19]
rem
rem  Switch DWM backdrop material interactively, to judge by eye which
rem  one looks closest to liquid glass. Pick a number, the window reopens.
rem
rem  Why a switch instead of a single value: CSS backdrop-filter cannot
rem  sample pixels behind the OS window (Chromium only filters content in
rem  its own render context), so the ONLY source of blur radius is DWM.
rem  The only thing worth experimenting with is the material.
rem
rem  Paths are relative to this script. Pure ASCII, no BOM, CRLF.
rem ===================================================================

set "HERE=%~dp0"
set "EXE=%HERE%..\src\ui\controller\RAMBlockCache.Controller\bin\Release\net10.0-windows\RAMBlockCache.Controller.exe"

if not exist "%EXE%" (
  echo [FAIL] controller exe not found:
  echo        %EXE%
  echo        run: build\build.ps1 -Target ui
  pause
  exit /b 1
)

:menu
cls
echo ============================================================
echo   DWM glass material probe
echo ============================================================
echo.
echo   1  transient   Desktop Acrylic  ^(current default, heavy frost^)
echo   2  none        no system material ^(test: is desktop sharp?^)
echo   3  mica        Mica             ^(opaque, for contrast only^)
echo   4  tabbed      Mica Alt         ^(opaque, for contrast only^)
echo   5  acrylic     WCA old channel  ^(Win10 fallback path^)
echo.
echo   0  quit
echo.
set /p "PICK=select and press Enter: "

if "%PICK%"=="1" set "MODE=transient"
if "%PICK%"=="2" set "MODE=none"
if "%PICK%"=="3" set "MODE=mica"
if "%PICK%"=="4" set "MODE=tabbed"
if "%PICK%"=="5" set "MODE=acrylic"
if "%PICK%"=="0" exit /b 0
if not defined MODE goto menu

echo.
echo [launch] RB_GLASS_MODE=%MODE%
set "RB_GLASS_MODE=%MODE%"
start "" "%EXE%"

timeout /t 11 /nobreak >nul
tasklist /FI "IMAGENAME eq RAMBlockCache.Controller.exe" | find /I "RAMBlockCache" >nul
if errorlevel 1 (
  echo [WARN] process not found - it may have exited
) else (
  echo [ok] window is up. look at the glass, then press any key to stop it.
  pause >nul
  taskkill /F /IM RAMBlockCache.Controller.exe >nul 2>&1
)

set "MODE="
set "RB_GLASS_MODE="
echo.
echo last DWM line in log:
findstr /C:"[284/DWM]" "%ProgramData%\RAMBlockCache\logs\ctrl-20261119.log" 2>nul | more +1
echo.
pause
goto menu