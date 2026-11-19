@echo off
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set OUT=%~dp0env_report.txt
(
  echo === testsigning ===
  bcdedit /enum ^| findstr /i testsigning
  echo === service query ===
  sc query RAMBlockCache
  echo === service config ===
  sc qc RAMBlockCache
  echo === UpperFilters Volume class ===
  reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}" /v UpperFilters
  echo === driver in system32 ===
  dir "%SystemRoot%\System32\drivers\RAMBlockCache.sys"
  echo === ProgramFiles install ===
  dir "%ProgramFiles%\RAMBoostFS"
  echo === verifier settings ===
  verifier /querysettings
) > "%OUT%" 2>&1
echo CHECK_DONE
