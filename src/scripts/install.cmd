@echo off
chcp 65001 >nul
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "try { Start-Process -FilePath '%~f0' -Verb RunAs } catch { Write-Host ('ELEVATE FAIL: ' + $_.Exception.Message); pause }"
  exit /b
)
cd /d "%~dp0"
REM [P1-7] 入口修正：旧链 install.cmd -> install_auto2.ps1 -> 无条件 exit 1
REM （install_auto2.ps1 是刻意废弃的占位脚本），用户双击只得到报错窗口。
REM 现指向 scripts\install.ps1（与 deploy.ps1 哈希门禁同模型的权威安装入口：
REM 编译->签名->注册服务->Volume UpperFilters->重枚举）。
echo [elevated] running install.ps1 ...
REM [P3] 去掉 -NoExit：install.ps1 末尾已有 Read-Host 保持窗口，
REM -NoExit 只会在脚本结束后多留一个空 powershell 窗口。
powershell -NoProfile -ExecutionPolicy Bypass -File "install.ps1"
