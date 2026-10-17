@echo off
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program1\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"
set EXE=%1
set FAIL=0
for /L %%s in (1,1,200) do (
  %EXE% %%s > build\seed_%%s.out 2>&1
  if errorlevel 1 (
    echo [seed %%s] FAIL
    type build\seed_%%s.out
    set /a FAIL+=1
  )
)
echo ==== seeds 1..200 FAIL=!FAIL! ====
