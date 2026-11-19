@echo off
rem ============================================================
rem  verifier_disable.cmd - reset Driver Verifier (P1.4)
rem  Pure ASCII + CRLF. Elevated console required.
rem  Reset clears ALL verifier settings for all drivers.
rem  Reboot after reset to return to normal operation.
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [X] Run as Administrator.
  exit /b 1
)

echo [*] Resetting Driver Verifier ...
verifier /reset
if %errorlevel% neq 0 (
  echo [X] verifier reset failed (exit %errorlevel%).
  exit /b 1
)
echo [ok] Verifier reset. REBOOT to return to normal operation.
exit /b 0
