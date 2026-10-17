@echo off
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting administrator privileges...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "ST=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
set "SHA=DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431"
set "F=%~dp0ui\wpf\bin\Release\net8.0-windows\win-x64\publish\RamBoostFS.Wpf.exe"
"%ST%" sign /sm /s My /sha1 %SHA% /fd SHA256 "%F%"
echo.
echo SIGN_WPF_SINGLE_DONE
pause
