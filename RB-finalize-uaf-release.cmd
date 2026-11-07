@echo off
rem ============================================================
rem  DEPRECATED 2026-11-07 - THIS WRAPPER REFUSES TO RUN.
rem  It targets finalize_uaf_release.ps1, a ONE-SHOT script whose hardcoded
rem  sys/Setup pins describe the 2026-11-06 UAF release. Replaying it now
rem  aborts at step 1 and can leave a half-signed state.
rem  USE INSTEAD: RB-finalize-geom-rebuild.cmd
rem ============================================================
echo [X] DEPRECATED wrapper. Use RB-finalize-geom-rebuild.cmd instead.
echo [X] Nothing was changed. No UAC was requested.
exit /b 1
