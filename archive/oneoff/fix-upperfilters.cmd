@echo off
REM ==========================================================================
REM  fix-upperfilters.cmd  (2026-11-01)
REM  修正 F5 修复的类 GUID 错误：RAMBlockCache 被注册到 DiskDrive 类（误），
REM  导致驱动挂在磁盘设备上（partmgr -> RAMBlockCache -> disk），
REM  卷级数据面永不接通。正确位置是 Storage Volume 类（与 volsnap 同栈）。
REM  依据：src/dist/install_manual.ps1 / deploy.ps1 / boot_guard.ps1 均用 71a27cdd
REM ==========================================================================
chcp 437 >nul
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
    echo [i] Administrator required, elevating...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set DISKCLS=HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}
set VOLCLS=HKLM\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}
if not exist build mkdir build >nul 2>&1
set TS=%DATE:~0,4%%DATE:~5,2%%DATE:~8,2%-%TIME:~0,2%%TIME:~3,2%%TIME:~6,2%
set TS=%TS: =0%

echo === BEFORE ===
echo [DiskDrive 4d36e967]
reg query "%DISKCLS%" /v UpperFilters
echo [StorageVolume 71a27cdd]
reg query "%VOLCLS%" /v UpperFilters

echo.
echo [1/4] Backup to build\uf-backup-%TS%.reg
reg export "%DISKCLS%" "build\uf-backup-%TS%-disk.reg" /y >nul
reg export "%VOLCLS%"  "build\uf-backup-%TS%-vol.reg"  /y >nul
echo      done

echo [2/4] DiskDrive: remove RAMBlockCache (keep partmgr)
reg add "%DISKCLS%" /v UpperFilters /t REG_MULTI_SZ /d partmgr /f >nul
if errorlevel 1 (echo      FAILED & pause & exit /b 1)
echo      done

echo [3/4] StorageVolume: append RAMBlockCache (keep volsnap)
reg add "%VOLCLS%" /v UpperFilters /t REG_MULTI_SZ /d "volsnap\0RAMBlockCache" /f >nul
if errorlevel 1 (echo      FAILED & pause & exit /b 1)
echo      done

echo [4/4] Verify
echo.
echo === AFTER ===
echo [DiskDrive 4d36e967]
reg query "%DISKCLS%" /v UpperFilters
echo [StorageVolume 71a27cdd]
reg query "%VOLCLS%" /v UpperFilters

echo.
echo ---------------------------------------------------------------
echo  Expected:
echo    DiskDrive      UpperFilters = partmgr
echo    StorageVolume  UpperFilters = volsnap RAMBlockCache
echo.
echo  REBOOT is required. After reboot run:
echo    verify-live.cmd   (run as administrator)
echo.
echo  If the machine fails to boot, run rollback-upperfilters.cmd
echo  from Safe Mode or WinRE Command Prompt.
echo ---------------------------------------------------------------
pause
