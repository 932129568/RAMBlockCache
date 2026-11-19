@echo off
REM deploy.cmd - self-elevate then run deploy.ps1, propagating its exit code.
REM NOTE: in a parenthesized block, %errorlevel% expands at parse time and
REM picks up the pre-net-session value, misreporting success as failure.
REM Use !errorlevel! (delayed expansion) instead.
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0deploy.ps1\"'; exit $p.ExitCode"
  set "RC=!errorlevel!"
  exit /b !RC!
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1"
exit /b %errorlevel%
