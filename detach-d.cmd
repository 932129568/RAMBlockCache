@echo off
REM detach-d.cmd (rev3) - cache integrity check + turn OFF caching on D:.
REM All Unicode/path work is done inside PowerShell (rev2 failed: chcp 437 mangles
REM the Chinese folder name, so the DLL lookup returned nothing and it exited early
REM without even writing a log).
chcp 65001 >nul
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
    echo [i] Administrator required - requesting UAC elevation...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" >nul 2>&1
    exit /b
)

echo === cache integrity check on D: ===
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build\wechat_check.ps1"
echo.
echo ==== done - log: build\wechat-check.txt ====
echo.
echo Now try starting WeChat:
echo   starts fine  -^> the cache was serving stale bytes (driver bug)
echo   still fails  -^> WeChat install is broken, reinstall 4.1.15.56
echo.
pause
