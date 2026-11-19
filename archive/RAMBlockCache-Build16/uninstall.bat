@echo off
REM =====================================================================
REM [ARCHIVE - Build 16] HISTORICAL COPY - DO NOT USE.
REM This snapshot is kept only as a ROLLBACK REFERENCE: read-only, do not
REM delete (refactor spec E.6 / P1-5).
REM Current uninstall: scripts\uninstall.cmd -> uninstall.ps1
REM (stops the service, removes the UpperFilters entry keeping volsnap).
REM This header is the ONLY change made to this snapshot file.
REM =====================================================================
echo === RAMBlockCache Uninstaller ===
echo This removes UpperFilters entry and stops the service.
pause
sc stop RAMBlockCache
sc delete RAMBlockCache
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}" /v UpperFilters /t REG_MULTI_SZ /d "volsnap" /f
schtasks /delete /tn RAMBoostBootGuard /f
schtasks /delete /tn RAMBoostLogonDisarm /f
del /f "%SystemRoot%\System32\drivers\RAMBlockCache.sys"
echo.
echo === Done. Reboot required. ===
pause