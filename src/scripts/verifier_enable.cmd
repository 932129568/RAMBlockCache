@echo off
rem ============================================================
rem  verifier_enable.cmd - Driver Verifier for RAMBlockCache (P1.4)
rem  2026-11-06. Pure ASCII + CRLF (C1 discipline).
rem
rem  PURPOSE: upgrade "gated + window closed" to instruction-level
rem  attribution. Verifier bugchecks ON PURPOSE at the fault point -
rem  EXPECT POSSIBLE BSODs. That is the harvest, not a failure.
rem
rem  BEFORE rebooting: keep offline_rollback.cmd at hand (WinRE).
rem  AFTER the cycle  : run verifier_disable.cmd, reboot again.
rem
rem  REQUIRES: elevated console. A reboot activates the settings.
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [X] Run as Administrator.
  exit /b 1
)

echo [*] Enabling Driver Verifier (standard flags) for RAMBlockCache ...
verifier /standard /driver RAMBlockCache.sys
if %errorlevel% neq 0 (
  echo [X] verifier enable failed (exit %errorlevel%).
  echo     Try: verifier /query  to inspect current state.
  exit /b 1
)

echo [ok] Verifier enabled for RAMBlockCache. REBOOT to activate.
echo [*] Suggested cycle: reboot - run workload (apps + benchmark.ps1)
echo     - let it run 15-60 min - collect dumps - disable - reboot.
echo [*] Dumps land in C:\Windows\Minidump\*.dmp or C:\Windows\MEMORY.DMP.
echo [!] Boot loop escape (WinRE console):
echo     reg load HKLM\TMPSYS C:\Windows\System32\config\SYSTEM
echo     reg delete "HKLM\TMPSYS\ControlSet001\Control\Session Manager\Memory Management" /v VerifyDrivers /f
echo     reg unload HKLM\TMPSYS
echo     (then run offline_rollback.cmd and reboot)
exit /b 0
