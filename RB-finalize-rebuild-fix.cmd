@echo off
rem ============================================================
rem  RB-finalize-rebuild-fix.cmd - ONE UAC deploy chain (94 spec)
rem  2026-11-08 UNFROZEN: 94-spec implementation (a648932/fb8a54f)
rem  passed independent review SPEC OK
rem  (deliverables/qa-rebuild-spec-96-review-2026-11-07.md).
rem  Gate is now conditional on the lead-created lift file:
rem    C:\ProgramData\rb-deploy\94-SPEC-OK-LIFTED.ok
rem  If it is missing this wrapper exits WITHOUT elevation (fail-closed).
rem  Chain: sign rebuilt sys -> dist + sidecar -> 3 anchors -> gate5 ->
rem  rebuild+sign Setup -> payload assertions -> deploy.ps1 (CHILD) ->
rem  post gate5 -> ledger -> reboot marker written LAST.
rem  Log: C:\ProgramData\rb-deploy\finalize-rebuild-fix.log
rem  AFTER UAC: lead runs sync_setup_hash.ps1 -RequireSetupChanged,
rem  then ONE reboot AND AN INTERACTIVE LOGON (logon_disarm must run
rem  to clear FilterArmed), then verify_final_reboot.ps1 and the S4
rem  five-tier matrix (4K/8K/16K/32K/64K).
rem  If System32 was locked, a queued replace may need a SECOND reboot.
rem  DO NOT re-run any full-chain finalize wrapper afterwards.
rem ============================================================
if not exist "C:\ProgramData\rb-deploy\94-SPEC-OK-LIFTED.ok" (
  echo [X] FROZEN: 94-SPEC-OK-LIFTED.ok missing - DO NOT DEPLOY.
  echo     31cf870/76aacfc/931ed53 frozen by 93 SPEC NO-GO; the 94-spec
  echo     implementation is SPEC OK but deploy stays fail-closed until
  echo     the lead manually creates the lift file. No UAC is requested.
  exit /b 1
)
echo [i] 94 SPEC OK lift present - proceeding.
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [..] Requesting elevation ...
  powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0src\scripts\finalize_rebuild_fix.ps1\"'"
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\scripts\finalize_rebuild_fix.ps1"
exit /b %errorlevel%
