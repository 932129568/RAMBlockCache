@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
    echo [i] 需要管理员权限，正在请求提权...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set EXE=src\tools\rbprobe\bin\Release\net10.0-windows\win-x64\rbprobe.exe
set OUT=build\verify-live.txt
if not exist build mkdir build >nul 2>&1
if not exist "%EXE%" (
    echo [x] 找不到探针 %EXE%，先执行: dotnet build src\tools\rbprobe\rbprobe.csproj -c Release
    pause
    exit /b 1
)

echo === RAMBlockCache 真机验证 %DATE% %TIME% === > "%OUT%"

echo. >> "%OUT%"
echo [阶段1] 驱动统计基线 >> "%OUT%"
"%EXE%" >> "%OUT%" 2>&1

echo. >> "%OUT%"
echo [阶段2] 造 IO（D 盘 128MiB，写后读两遍） >> "%OUT%"
"%EXE%" --bench D --size 128 >> "%OUT%" 2>&1

echo. >> "%OUT%"
echo [阶段3] 统计复查（对比阶段1，看 ReadReq/WriteReq/CurBytes 是否增长） >> "%OUT%"
"%EXE%" >> "%OUT%" 2>&1

type "%OUT%"
echo.
echo 结果已保存: %CD%\%OUT%
pause
