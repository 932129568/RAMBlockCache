@echo off
rem ==================================================================
rem DEPRECATED - sign_wpf_single.cmd is intentionally retired [FIX S-09].
rem Reason (code-standards 7.3): the old target
rem   ui\wpf\...\RamBoostFS.Wpf.exe
rem is a legacy NON-release artifact; fixing only its path discovery
rem would just make a stale target fail more prettily.
rem Authoritative entry: sign_all.cmd in this same directory.
rem   SIGTOOL env var first, then Windows Kits descending version scan;
rem   exits 1 if no signtool is found. It signs the real release
rem   artifacts: RAMBlockCache.sys / RAMBlockCache.Controller.exe /
rem   ramboostctl.exe (missing targets are skipped).
rem This stub always exits 1 ("script intentionally deprecated").
rem ==================================================================
echo [DEPRECATED] sign_wpf_single.cmd is retired; use sign_all.cmd.
echo   Old target RamBoostFS.Wpf.exe is not a release artifact.
echo   sign_all.cmd signs the real release artifacts (sys / Controller.exe / ctl).
exit /b 1
