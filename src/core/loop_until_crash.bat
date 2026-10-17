@echo off
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program1\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"
set EXE=%1
set MAX=%2
set i=1
:loop
if %i% gtr %MAX% goto done
%EXE% > build\crash_%i%.log 2>&1
set ec=!ERRORLEVEL!
findstr /C:"AddressSanitizer" build\crash_%i%.log >nul
if !errorlevel! equ 0 (
  echo [iter %i] ASAN-CRASH exit=!ec!
  goto done
)
if !ec! neq 0 (
  echo [iter %i] NONZERO exit=!ec!
  type build\crash_%i%.log
  goto done
)
set /a i+=1
goto loop
:done
echo stopped at iter %i%
