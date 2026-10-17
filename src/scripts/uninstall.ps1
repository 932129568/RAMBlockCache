# RAMBlockCache 卸载 V10
# 顺序：停止服务（DriverUnload 会 flush 全部脏数据）→ 上层过滤器摘除（保留 volsnap）
#      → 删服务 → 删驱动文件 → 提示重启（重启后过滤器彻底退出所有卷栈）
#Requires -RunAsAdministrator
$ErrorActionPreference = "Stop"

Write-Host "== RAMBlockCache uninstaller V10 =="

# 1) 停服务（干净卸载：flush 全部脏块，回写窗口清零）
$s = Get-Service RAMBlockCache -ErrorAction SilentlyContinue
if ($s) {
    if ($s.Status -ne "Stopped") { Stop-Service RAMBlockCache -Force; Start-Sleep 2 }
    Write-Host "[ok] service stopped (dirty flushed by DriverUnload)"
}

# 2) 上层过滤器摘除（读-改-写，保留 volsnap）
$cls = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}"
$v = (Get-ItemProperty -Path $cls -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
if ($v) {
    $keep = @($v | Where-Object { $_ -ne "RAMBlockCache" })
    if ($keep.Count -gt 0) { Set-ItemProperty -Path $cls -Name UpperFilters -Value $keep -Type MultiString }
    else                   { Remove-ItemProperty -Path $cls -Name UpperFilters }
    Write-Host "[ok] UpperFilters cleaned (volsnap kept): $($keep -join ',')"
}
# 旧非标准 GUID 残留清理（BUG-009）
$bad = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{536670a4-367c-463f-8b14-fa40f6d6fc1f}"
if (Test-Path $bad) {
    $bv = (Get-ItemProperty -Path $bad -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
    if ($bv) { Set-ItemProperty -Path $bad -Name UpperFilters -Value @($bv | Where-Object { $_ -ne "RAMBlockCache" }) -Type MultiString }
    Write-Host "[ok] legacy class GUID cleaned"
}

# 3) 删服务、删文件
if ($s) { sc.exe delete RAMBlockCache | Out-Null; Write-Host "[ok] service deleted" }
$drv = "$env:SystemRoot\System32\drivers\RAMBlockCache.sys"
if (Test-Path $drv) { Remove-Item $drv -Force; Write-Host "[ok] driver file removed" }

Write-Host ">>> DONE. Reboot to fully detach the filter from all volume stacks."
