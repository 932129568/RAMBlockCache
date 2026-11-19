@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
set N=%1
set EXE=%2
set FAIL=0
for /L %%i in (1,1,%N%) do (
  %EXE% > build\lc_%%i.out 2>&1
  if errorlevel 1 (
    echo [iter %%i] FAIL exit=!ERRORLEVEL!
    type build\lc_%%i.out
    set /a FAIL+=1
  )
)
echo ==== %N% iters, FAIL=!FAIL! ====
