@echo off
REM ============================================================
REM  offline_rollback.cmd  (V10.2 worst-case fallback, pure ASCII)
REM
REM  Use this ONLY if the machine will not boot after deploying the
REM  boot-start volume filter. It strips RAMBlockCache from the Volume
REM  class UpperFilters (keeping every other filter, incl. volsnap and
REM  any third-party filter) on the OFFLINE Windows registry hive, and
REM  clears the armed flag.
REM
REM  Usage (boot into WinRE):
REM    1. Boot from Windows install media / enter WinRE
REM       (Troubleshoot -> Advanced -> Command Prompt).
REM    2. Find the Windows drive (often not C: in WinRE).
REM       If it is NOT C:, edit the SYSROOT line below to match,
REM       e.g.  set SYSROOT=D:\Windows
REM    3. Run this script, e.g.:
REM          X:\path\to\offline_rollback.cmd
REM    4. Reboot. No network required.
REM ============================================================

setlocal EnableExtensions EnableDelayedExpansion

REM [P1-9] 探测当前引导实际使用的 ControlSet：Select\Current 是 DWORD
REM （0x1/0x2/0x3…），映射为 ControlSet001/002/003。
REM 旧脚本硬编码 ControlSet001：若机器曾引导 002/003（快照还原、双系统、
REM 多次装机残留），改写的注册表根本不是本次引导的集合 → 回滚无效 =
REM 系统继续 0x7B。探测失败才回退 ControlSet001（Windows 默认）。
set "CSD="
for /f "tokens=3" %%D in ('reg query "HKLM\RB_OFFLINE\Select" /v Current 2^>nul ^| findstr /i /c:"Current"') do set "_CSN=%%D"
if defined _CSN (
  if /i "%_CSN%"=="0x1" set "CSD=001"
  if /i "%_CSN%"=="0x2" set "CSD=002"
  if /i "%_CSN%"=="0x3" set "CSD=003"
)
if not defined CSD set "CSD=001"
set "CS=SYSTEM\ControlSet%CSD%"
echo [..] Target ControlSet: %CS%
set "CLS=%CS%\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}"

set "SYSROOT=C:\Windows"
set "HIVE=%SYSROOT%\System32\config\SYSTEM"

if not exist "%HIVE%" (
  echo [X] Offline SYSTEM hive not found: %HIVE%
  echo     Edit this script and set SYSROOT= to your Windows drive
  echo     (try D:, E: ...). Then re-run.
  exit /b 1
)

echo [..] Loading offline SYSTEM hive ...
reg load HKLM\RB_OFFLINE "%HIVE%" >nul
if errorlevel 1 (
  echo [X] reg load failed. Is the hive already loaded?
  echo     Try: reg unload HKLM\RB_OFFLINE   then re-run.
  exit /b 1
)

echo [..] Removing RAMBlockCache from Volume UpperFilters (keep other filters) ...
REM [P1-9] 只摘除 RAMBlockCache 条目，其余卷过滤器（含第三方）原样保留。
REM 旧脚本把 UpperFilters 重置为 volsnap-only，静默删除第三方过滤器；
REM 这是 0x7B 时的最后恢复手段，不能顺带破坏其他存储栈组件。
REM 摘除后为空时回落 Windows 默认值 volsnap（缺 UpperFilters 本身即
REM 可能导致 volsnap 不加载）。
powershell -NoProfile -ExecutionPolicy Bypass -Command "$p='HKLM:\RB_OFFLINE\%CLS%'; $v=(Get-ItemProperty -Path $p -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters; if($null -ne $v){ $keep=@($v | Where-Object { $_ -ne 'RAMBlockCache' }); if($keep.Count -eq 0){ $keep=@('volsnap') }; New-ItemProperty -Path $p -Name UpperFilters -PropertyType MultiString -Value $keep -Force | Out-Null; Write-Host ('[ok] UpperFilters now: ' + ($keep -join '; ')) } else { Write-Host '[..] UpperFilters not present, nothing to strip' }"

echo [..] Clearing armed flag in service Parameters ...
reg add "HKLM\RB_OFFLINE\%CS%\Services\RAMBlockCache\Parameters" /v FilterArmed /t REG_DWORD /d 0 /f >nul 2>&1

echo [..] Unloading hive ...
reg unload HKLM\RB_OFFLINE >nul

echo [OK] Rolled back. You can now reboot into Windows.
endlocal
