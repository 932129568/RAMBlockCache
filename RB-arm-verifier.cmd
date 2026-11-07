@echo off
rem ============================================================
rem  RB-arm-verifier.cmd - OPTIONAL Driver Verifier arming
rem  2026-11-08. Run ONLY AFTER verify_final_reboot.ps1 FAIL=0
rem  (the new signed sys verified clean on real machine).
rem  Arming standard verifier on RAMBlockCache.sys ONLY:
rem    special pool + force IRQL checking + pool tracking +
rem    random low-resources simulation (hits the F5 face 59 warned).
rem  Takes effect NEXT boot. A second reboot is needed after this.
rem  If the machine bugchecks twice in a row, BootLoopGuard refuses
rem  attach; recover with: verifier /reset  (admin, then reboot).
rem  This script is NOT part of the deploy chain and is opt-in.
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process cmd -Verb RunAs -ArgumentList '/c \"%~f0\"'"
  exit /b 0
)
echo [i] Arming Driver Verifier (standard) for RAMBlockCache.sys only...
verifier /standard /driver RAMBlockCache.sys
if %errorlevel% neq 0 (
  echo [X] verifier arming failed exit=%errorlevel%
  exit /b %errorlevel%
)
echo [i] Armed. Active settings:
verifier /querysettings
echo.
echo [OK] Verifier armed for next boot. Reboot to activate.
echo      Disable anytime (admin): verifier /reset  then reboot.
exit /b 0
