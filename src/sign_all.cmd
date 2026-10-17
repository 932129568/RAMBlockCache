@echo off
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting administrator privileges...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "ROOT=%~dp0"

rem [P2-9] signtool 自动发现：环境变量 SIGTOOL 优先；否则在 Windows Kits\10\bin
rem 下按版本号倒序取第一个存在的 x64\signtool.exe。
rem 旧脚本硬编码 10.0.26100.0，换机/换 SDK 版本即 "找不到文件"。
set "ST=%SIGTOOL%"
if not defined ST (
  for /f "delims=" %%d in ('dir /b /ad /o-n "C:\Program Files (x86)\Windows Kits\10\bin" 2^>nul') do (
    if not defined ST if exist "C:\Program Files (x86)\Windows Kits\10\bin\%%d\x64\signtool.exe" set "ST=C:\Program Files (x86)\Windows Kits\10\bin\%%d\x64\signtool.exe"
  )
)
if not defined ST (
  echo [X] signtool.exe not found. Set SIGTOOL=C:\path\to\signtool.exe and re-run.
  pause
  exit /b 1
)
echo using signtool: %ST%
set "SHA=DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431"

rem 从 common\version.h 读版本号（单一事实源，不字面写死）
set "VER="
for /f "tokens=3" %%a in ('findstr /b "#define RB_VERSION_MAJOR " "%ROOT%common\version.h"') do set "VMAJ=%%a"
for /f "tokens=3" %%a in ('findstr /b "#define RB_VERSION_MINOR " "%ROOT%common\version.h"') do set "VMIN=%%a"
for /f "tokens=3" %%a in ('findstr /b "#define RB_VERSION_PATCH " "%ROOT%common\version.h"') do set "VPAT=%%a"
for /f "tokens=3" %%a in ('findstr /b "#define RB_VERSION_BUILD " "%ROOT%common\version.h"') do set "VBLD=%%a"
set "VER=%VMAJ%.%VMIN%.%VPAT%.%VBLD%"
echo ===== RAMBlockCache version %VER% (from common\version.h) =====

rem [P2-9] 签名目标修正为实际发布物：
rem   driver .sys / dist\wpf\RAMBlockCache.Controller.exe / ctl。
rem 旧目标 setup\RamBoostSetup.exe 不存在（实为 .ps1，无需签名）；
rem ui\wpf\...\RamBoostFS.Wpf.exe 是旧代非发布物。
set "SYS=%ROOT%driver\x64\Release\RAMBlockCache.sys"
set "CTL=%ROOT%dist\wpf\RAMBlockCache.Controller.exe"
if not exist "%CTL%" set "CTL=%ROOT%ui\controller\RAMBlockCache.Controller\bin\Release\net8.0-windows\publish\RAMBlockCache.Controller.exe"
set "CLI=%ROOT%ctl\ramboostctl.exe"

call :sign1 "driver .sys" "%SYS%"
call :sign1 "controller .exe" "%CTL%"
call :sign1 "ctl .exe" "%CLI%"

echo.
echo ===== Verify driver signature =====
if exist "%SYS%" "%ST%" verify /v /all "%SYS%"
echo.
echo ===== Signed RAMBlockCache version %VER% =====
echo SIGN_ALL_DONE
pause
exit /b 0

:sign1
if exist %2 (
  echo [sign] %~1
  "%ST%" sign /sm /s My /sha1 %SHA% /fd SHA256 %2
) else (
  echo [skip] %~1 not found: %2
)
exit /b 0
