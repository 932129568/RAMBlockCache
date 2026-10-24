# RAMBlockCache 卸载 V10
# 顺序：停止服务（DriverUnload 会 flush 全部脏数据）→ 上层过滤器摘除（保留 volsnap）
#      → 删服务 → 删驱动文件 → 提示重启（重启后过滤器彻底退出所有卷栈）
#Requires -RunAsAdministrator
$ErrorActionPreference = "Stop"

Write-Host "== RAMBlockCache uninstaller V10 =="

# 1) 停服务（干净卸载：flush 全部脏块，回写窗口清零）
# [FIX S7-18] 停服必须确认真正停止：原实现 Stop-Service -Force + Start-Sleep 2 即继续
# ——驱动按设计在"停服超时（~15s 排不净在途 I/O）"时中止卸载保数据，此时服务仍在运行，
# 后续摘 UpperFilters/删服务/删 sys 会把脏数据未回写的驱动拆栈（用户重启后丢已确认写入，
# 与安装器 GUI [FIX 高-1] 同型缺陷在脚本侧未同步）。现按 install.ps1:48-54 同款
# 15s 有界确认，未停止即 fail-closed 中止（系统保持原状）。
$s = Get-Service RAMBlockCache -ErrorAction SilentlyContinue
if ($s) {
    if ($s.Status -ne "Stopped") {
        Stop-Service RAMBlockCache -Force
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $stopped = $false
        while ($sw.Elapsed.TotalSeconds -lt 15) {
            Start-Sleep -Milliseconds 500
            $s.Refresh()
            if ($s.Status -eq "Stopped") { $stopped = $true; break }
        }
        if (-not $stopped) {
            Write-Host "[X] service stop timeout (~15s): driver aborted unload by design to protect dirty data."
            Write-Host "    Reboot once, then re-run this uninstaller. System left unchanged."
            exit 1
        }
    }
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

# 2b) [FIX S7-22] 摘武装基线与看门计划任务：原卸载不清 FilterArmed 与两个
# 计划任务——已卸载产品若将来因故重新武装（boot_guard 见 FilterArmed=1 且
# 确认启动失败）会回滚 UpperFilters；看门任务继续为已卸载产品看门。
$svcKey = "HKLM://SYSTEM//CurrentControlSet//Services//RAMBlockCache"
if (Test-Path $svcKey) {
    Set-ItemProperty -Path $svcKey -Name FilterArmed -Value 0 -Type DWord -ErrorAction SilentlyContinue
    Write-Host "[ok] FilterArmed baseline cleared"
}
# 任务名与 deploy.ps1:490/496 注册的一致（RAMBoost 前缀）
foreach ($task in @("RAMBoostBootGuard", "RAMBoostLogonDisarm")) {
    $null = schtasks /Delete /TN $task /F 2>$null
    if ($LASTEXITCODE -eq 0) { Write-Host "[ok] scheduled task removed: $task" }
}

# 3) 删服务、删文件
if ($s) { sc.exe delete RAMBlockCache | Out-Null; Write-Host "[ok] service deleted" }
$drv = "$env:SystemRoot\System32\drivers\RAMBlockCache.sys"
if (Test-Path $drv) { Remove-Item $drv -Force; Write-Host "[ok] driver file removed" }

Write-Host ">>> DONE. Reboot to fully detach the filter from all volume stacks."
