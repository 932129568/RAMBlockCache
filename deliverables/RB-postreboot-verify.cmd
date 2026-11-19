@echo off
rem ===========================================================================
rem  RB-postreboot-verify.cmd
rem  RAMBlockCache POST-REBOOT VERIFY - self-elevates ONCE via UAC.
rem
rem  WHEN: after  src\scripts\deploy.cmd  AND  exactly one reboot.
rem  WHAT: read-only. No refresh / no flags write / no l2 enable / no shrink /
rem        no rollback / no reboot / no bootcfg POST / no registry or file write.
rem        The only file touched is the log under C:\ProgramData\rb-deploy\.
rem  HOW : runs RB-postreboot-verify.ps1 (same folder) with pwsh 7 when present,
rem        else Windows PowerShell. Output goes to console AND to the log.
rem
rem  Exit code of the .ps1 = number of HARD failures (0 = all green).
rem ===========================================================================
setlocal EnableExtensions
chcp 65001 >nul

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

set "DEPLOY=C:\ProgramData\rb-deploy"
set "LOG=%DEPLOY%\postreboot-verify.log"
set "PS1=%~dp0RB-postreboot-verify.ps1"

if not exist "%DEPLOY%" mkdir "%DEPLOY%" >nul 2>&1
if not exist "%PS1%" (
  echo [X] missing companion script: "%PS1%"
  pause
  exit /b 9
)

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
  echo.
  echo *** NOT ELEVATED *** requesting UAC ...
  echo     the elevated window runs the same checks and stays open.
  set "RB_SELF=%~f0"
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:RB_SELF -Verb RunAs"
  exit /b 0
)

echo === RAMBlockCache POST-REBOOT VERIFY (READ-ONLY) === > "%LOG%"
echo cmd started : %DATE% %TIME% >> "%LOG%"
echo repo        : %~dp0.. >> "%LOG%"
echo. >> "%LOG%"

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%PS1%" >> "%LOG%" 2>&1
set "RC=%ERRORLEVEL%"

type "%LOG%"
echo.
echo log  : %LOG%
echo exit : %RC%   (0 = all hard checks passed)
echo.
pause
exit /b %RC%
