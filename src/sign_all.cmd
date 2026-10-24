@echo off
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting administrator privileges...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "ROOT=%~dp0"

rem [FIX S-12] REM comments English-ized (batch encoding discipline).
rem [P2-9] signtool auto-discovery: env var SIGTOOL takes priority; otherwise
rem take the first existing x64\signtool.exe under Windows Kits\10\bin in
rem descending version order.
rem The old script hardcoded 10.0.26100.0, so a different machine or SDK
rem version meant "file not found".
set "ST=%SIGTOOL%"
if not defined ST (
  for /f "delims=" %%d in ('dir /b /ad /o-n "C:\Program Files (x86)\Windows Kits\10\bin" 2^>nul') do (
    if not defined ST if exist "C:\Program Files (x86)\Windows Kits\10\bin\%%d\x64\signtool.exe" set "ST=C:\Program Files (x86)\Windows Kits\10\bin\%%d\x64\signtool.exe"
  )
)
if not defined ST (
  echo [X] signtool.exe not found. Set SIGTOOL=C:\path\to\signtool.exe and re-run.
  pause
  exit /b 1
)
echo using signtool: %ST%
set "SHA=DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431"

rem Read the version from common\version.h (single source of truth, not literals)
set "VER="
for /f "tokens=3" %%a in ('findstr /b "#define RB_VERSION_MAJOR " "%ROOT%common\version.h"') do set "VMAJ=%%a"
for /f "tokens=3" %%a in ('findstr /b "#define RB_VERSION_MINOR " "%ROOT%common\version.h"') do set "VMIN=%%a"
for /f "tokens=3" %%a in ('findstr /b "#define RB_VERSION_PATCH " "%ROOT%common\version.h"') do set "VPAT=%%a"
for /f "tokens=3" %%a in ('findstr /b "#define RB_VERSION_BUILD " "%ROOT%common\version.h"') do set "VBLD=%%a"
set "VER=%VMAJ%.%VMIN%.%VPAT%.%VBLD%"
echo ===== RAMBlockCache version %VER% (from common\version.h) =====

rem [P2-9] Signing targets corrected to the actual release artifacts:
rem   driver .sys / dist\wpf\RAMBlockCache.Controller.exe / ctl / setup engine /
rem   installer GUI / feature CLI.
rem [FIX R6-SIGN1 2026-10-21] Every sign/verify step checks its exit code and
rem any failure aborts with non-zero: the old script ignored signtool failures
rem and always printed SIGN_ALL_DONE + exit 0 -- the shipped user-mode binaries
rem (Setup.exe / ramboostsetup.exe / ramboostctl.exe / rbfeatctl.exe /
rem Controller.exe) were left NotSigned with no warning at all.
set "SYS=%ROOT%driver\x64\Release\RAMBlockCache.sys"
set "CTL=%ROOT%dist\wpf\RAMBlockCache.Controller.exe"
if not exist "%CTL%" set "CTL=%ROOT%ui\controller\RAMBlockCache.Controller\bin\Release\net8.0-windows\publish\RAMBlockCache.Controller.exe"
set "CLI=%ROOT%ctl\ramboostctl.exe"
set "FEAT=%ROOT%ctl\rbfeatctl.exe"
set "SETUP=%ROOT%setup\ramboostsetup.exe"
set "INSTALLER=%ROOT%dist\RAMBlockCache-Setup.exe"
if not exist "%INSTALLER%" set "INSTALLER=%ROOT%ui\installer\RAMBlockCache.Setup\bin\Release\net8.0-windows\RAMBlockCache.Setup.exe"

set "SIGNFAIL=0"
call :sign1 "driver .sys" "%SYS%"
if errorlevel 1 set /a SIGNFAIL+=1
call :sign1 "controller .exe" "%CTL%"
if errorlevel 1 set /a SIGNFAIL+=1
call :sign1 "ctl .exe" "%CLI%"
if errorlevel 1 set /a SIGNFAIL+=1
call :sign1 "feature ctl .exe" "%FEAT%"
if errorlevel 1 set /a SIGNFAIL+=1
call :sign1 "setup engine .exe" "%SETUP%"
if errorlevel 1 set /a SIGNFAIL+=1
call :sign1 "installer .exe" "%INSTALLER%"
if errorlevel 1 set /a SIGNFAIL+=1

echo.
echo ===== Verify driver signature =====
if exist "%SYS%" (
  "%ST%" verify /v /all "%SYS%"
  if errorlevel 1 (
    echo [X] driver signature verification FAILED.
    set /a SIGNFAIL+=1
  )
)

echo.
if %SIGNFAIL% gtr 0 (
  echo [X] SIGN_ALL_INCOMPLETE: %SIGNFAIL% target^(s^) failed. Fix the errors above and re-run.
  pause
  exit /b 1
)
echo ===== Signed RAMBlockCache version %VER% =====
echo SIGN_ALL_DONE
pause
exit /b 0

:sign1
if exist %2 (
  echo [sign] %~1
  "%ST%" sign /sm /s My /sha1 %SHA% /fd SHA256 %2
  if errorlevel 1 (
    echo [X] sign FAILED: %~1 (%2^)
    exit /b 1
  )
) else (
  echo [skip] %~1 not found: %2
)
exit /b 0
