@echo off
rem ============================================================
rem  DEPRECATED - DO NOT DOUBLE-CLICK. See deliverables/71-harvest-closeout.
rem  This 18:04 script aborted after 'vite build' in an elevated
rem  PowerShell 5.1 session ($ErrorActionPreference='Stop' + native
rem  stderr via 2>&1 -> terminating NativeCommandError).
rem  Replacement: RB-finalize-final.cmd (sign only, no rebuild).
rem ============================================================
echo [X] RB-finalize-harvest.cmd is DEPRECATED.
echo     It aborts mid-build. Use RB-finalize-final.cmd instead.
echo     See deliverables\71-harvest-closeout-2026-11-06.md
exit /b 1
