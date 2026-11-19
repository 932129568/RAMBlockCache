@echo off
rem ===========================================================================
rem  RB-r2-restore-step2.cmd
rem  STEP 2 of 2: run AFTER the single reboot following step 1.
rem  Calls phase4-pathB.ps1 -Step VerifyRestore. All 9 checks must be True.
rem  READ ONLY: no deploy, no registry write, no reboot, no git commit.
rem
rem  Do NOT run this before the reboot; VerifyRestore would then be measuring
rem  the pre-reboot state and could produce a misleading result.
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"
set "STAGE=C:\ProgramData\rb-deploy\phase4-stage"
set "RUNNER=%STAGE%\phase4-pathB.ps1"
set "LOG=C:\ProgramData\rb-deploy\r2-verifyrestore.log"
for %%I in ("%~dp0..") do set "REPO=%%~fI"
set "RB_SELF=%~f0"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting one UAC prompt ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:RB_SELF -Verb RunAs"
    exit /b 0
)

if not exist "%RUNNER%" goto :no_runner
if not exist "C:\ProgramData\rb-deploy" mkdir "C:\ProgramData\rb-deploy"

echo ==================================================================
echo   R2 VERIFY RESTORE - elevated, READ ONLY
echo ==================================================================
echo repo : %REPO%
echo log  : %LOG%
echo.
"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%RUNNER%" -Step VerifyRestore -Repo "%REPO%" > "%LOG%" 2>&1
set "RC=%ERRORLEVEL%"
type "%LOG%"
echo.
echo VERIFYRESTORE_EXIT = %RC%
echo CLEAN requires all 9 checks True. Send %LOG% back for the verdict.
echo R7: until the log confirms it, the R2 window is NOT closed.
pause
exit /b %RC%

:no_runner
echo [STOP] runner not found: %RUNNER%
pause
exit /b 2
