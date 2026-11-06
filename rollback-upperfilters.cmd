@echo off
REM ==========================================================================
REM  rollback-upperfilters.cmd
REM  紧急回滚：把两个卷/磁盘类的 UpperFilters 恢复为 Windows 默认，
REM  即完全不含 RAMBlockCache（驱动仍加载但不再挂栈 = 纯透传，系统必可启动）。
REM  用法：安全模式 / WinRE 命令提示符，双击或在 cmd 中运行。
REM        本脚本不需要项目文件完整，只要 reg.exe 可用。
REM ==========================================================================
chcp 437 >nul
setlocal

net session >nul 2>&1
if errorlevel 1 (
    echo [i] Administrator required, elevating...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set DISKCLS=HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}
set VOLCLS=HKLM\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}

echo [1/2] DiskDrive  -> partmgr
reg add "%DISKCLS%" /v UpperFilters /t REG_MULTI_SZ /d partmgr /f
echo [2/2] StorageVolume -> volsnap
reg add "%VOLCLS%" /v UpperFilters /t REG_MULTI_SZ /d volsnap /f

echo.
echo === Verify ===
reg query "%DISKCLS%" /v UpperFilters
reg query "%VOLCLS%" /v UpperFilters

echo.
echo Rollback done. Reboot to return to passthrough-only state.
pause
