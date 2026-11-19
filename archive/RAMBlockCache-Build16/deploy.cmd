@echo off
REM =====================================================================
REM [ARCHIVE - Build 16] HISTORICAL COPY - DO NOT USE FOR DEPLOYMENT.
REM This snapshot is kept only as a ROLLBACK REFERENCE: read-only, do not
REM delete (refactor spec E.6 / P1-5).
REM This old wrapper does not wait for / propagate the deploy script's
REM exit code. Current version: src\scripts\deploy.cmd (self-elevates,
REM waits via -Wait -PassThru, and propagates the child exit code with
REM !errorlevel! delayed expansion).
REM This header is the ONLY change made to this snapshot file.
REM =====================================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0deploy.ps1\"'"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1"
