@echo off
rem ===========================================================================
rem  RB-j6-postboot-probe.cmd
rem  J6 POST-BOOT READ-ONLY ROOT-CAUSE PROBE (D7 + D8).
rem  User-approved 2026-11-13, path C. Self-elevates ONCE via UAC.
rem
rem  READ-ONLY with respect to the DRIVER and its CONFIGURATION:
rem    no refresh, no flags write, no l2 enable/disable, no shrink,
rem    no rollback, no reboot, no verifier /reset, no method=2,
rem    no bootcfg POST, no write-back / WBCOAL change, no registry write.
rem  The only file written is the log under C:\ProgramData\rb-deploy\.
rem
rem  WHY: rbfeatctl stats -json exposes g_Rb2Flags (rbfeat.c:68). One read
rem  splits D7 into two mutually exclusive fixes:
rem    flags & 4 == 4  -> Flags WERE loaded; the post-boot L2 lazy-open
rem                       chain is broken (no attach -> no L2 probe).
rem    flags & 4 == 0  -> Rb2ReloadFlags never loaded them at boot.
rem  NEVER issue REFRESH here: it has side effects (rbfeat.c:718).
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting UAC ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

set "DEPLOY=C:\ProgramData\rb-deploy"
set "REPO=%~dp0.."
set "CTL=%REPO%\src\ctl\rbfeatctl.exe"
set "LOG=%DEPLOY%\j6-postboot-probe.log"

echo === J6 POST-BOOT READ-ONLY PROBE === > "%LOG%"
powershell -NoProfile -Command "Write-Output ('UTC=' + (Get-Date).ToUniversalTime().ToString('o'))" >> "%LOG%" 2>&1
powershell -NoProfile -Command "$b=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime(); Write-Output ('BOOT_UTC=' + $b.ToString('o'))" >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo --- 1) rbfeatctl stats -json (DECISIVE: flags) --- >> "%LOG%"
"%CTL%" stats -json >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo --- 2) rbfeatctl stats (text) --- >> "%LOG%"
"%CTL%" stats >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo --- 3) rbfeatctl l2 status --- >> "%LOG%"
"%CTL%" l2 status >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo --- 4) rbfeatctl vols --- >> "%LOG%"
"%CTL%" vols >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo --- 5) Features registry --- >> "%LOG%"
powershell -NoProfile -Command "Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features' | Select-Object Flags,L2Letter,L2SizeGB,ExcludeDirs | Format-List" >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo --- 6) Parameters registry (boot state) --- >> "%LOG%"
powershell -NoProfile -Command "Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters' | Select-Object AutoAttach,ResumeOnBoot,ResumeLetters,FilterArmed,BootFailCount,LastCleanShutdown,LastBootHealthy,DeploySeq,DeployAck,BootSeq,WriteBack,BlockSize | Format-List" >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo --- 7) UpperFilters --- >> "%LOG%"
powershell -NoProfile -Command "$k='HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'; Write-Output ('UpperFilters=' + ((Get-ItemProperty -LiteralPath $k).UpperFilters -join ','))" >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo --- 8) driver log tail (new boot section) --- >> "%LOG%"
powershell -NoProfile -Command "Get-Content -LiteralPath 'C:\ProgramData\RAMBlockCache\logs\driver-20261113.log' -Tail 60" >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo --- 9) L2 artifacts (size + mtime) --- >> "%LOG%"
powershell -NoProfile -Command "Get-Item -LiteralPath 'C:\RAMBoostL2c.idx','C:\RAMBoostL2c.bin','D:\rbc-l2-load.bin' -ErrorAction SilentlyContinue | Select-Object FullName,Length,LastWriteTimeUtc | Format-Table -AutoSize" >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo --- 10) running sys hash --- >> "%LOG%"
powershell -NoProfile -Command "(Get-FileHash -LiteralPath 'C:\Windows\System32\drivers\RAMBlockCache.sys' -Algorithm SHA256).Hash.ToLower()" >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo === PROBE COMPLETE (READ-ONLY) === >> "%LOG%"
type "%LOG%"
echo.
echo log: %LOG%
echo.
pause
