@echo off
rem ===========================================================================
rem  RB-l2-enable.cmd
rem  Open the L2 second-level cache on a volume, with Windows Verifier already
rem  running (Special pool + Pool tracking) so that UAF-class defects surface
rem  as a BSOD instead of silent corruption.
rem
rem  This creates  <Letter>:\RAMBoostL2c.bin  (size = the GB argument).
rem  Deleting that file deletes the cache only; no user data is touched.
rem  WriteBack stays 0 (write-through), so a BSOD does not lose written data.
rem
rem  Usage: right-click -> "Run as administrator"
rem  Log   : C:\ProgramData\rb-deploy\l2-enable-<timestamp>.log
rem
rem  Undo  : rbfeatctl l2 disable      (elevated, same folder as this script)
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED ***
    echo Right-click this file and choose "Run as administrator".
    echo.
    pause
    exit /b 5
)

echo ==================================================================
echo   RAMBlockCache: enable L2 cache  (Verifier is ON - expect BSOD
echo   if the L2 guard is broken; that is the point of this run)
echo ==================================================================
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0RB-l2-enable.ps1" -Letter D -SizeGB 2

set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
    echo *** L2 enable did not complete ^(exit=%RC%^) - see the log above ***
) else (
    echo *** l2 enable sent. Read the "after" status block to confirm state=opened. ***
)
echo.
pause
exit /b %RC%
