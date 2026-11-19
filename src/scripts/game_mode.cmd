@echo off
REM [FIX S-12] chcp 65001: the wrapped .ps1 emits UTF-8 Chinese output.
chcp 65001 >nul
REM game_mode.cmd - launcher wrapper for game_mode.ps1 (self-elevate + invoke).
REM Error-handling convention (spec D.3):
REM   * direct path: propagates the wrapped script's exit code (exit /b %errorlevel%).
REM     Identical to the implicit batch behavior (a batch file exits with the
REM     errorlevel of its last command); this line only makes the intent explicit.
REM   * elevation branch: the wrapped script is interactive (ends with Read-Host),
REM     so it is deliberately fire-and-forget, matching the existing uninstall.cmd
REM     pattern. For exit-code propagation on elevation, see deploy.cmd
REM     (Start-Process -Wait -PassThru + !errorlevel!).
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0game_mode.ps1\"'"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0game_mode.ps1"
exit /b %errorlevel%
