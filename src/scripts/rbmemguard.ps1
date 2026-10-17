# rbmemguard.ps1 - memory guard: while free physical memory < 8GB, ask the driver to
# shrink 15% every 5s.
# Error-handling convention (spec D.3): $ErrorActionPreference='Stop'.
# The loop body only calls a local exe and Start-Sleep. Under Windows PowerShell 5.1 a
# native command's failure does not throw (stderr passes through), so the loop semantics
# are unchanged from before the refactor; only a CommandNotFoundException (exe missing)
# now terminates with a non-zero exit instead of repeating the error forever. PowerShell
# 7+ would also turn exe stderr into errors only if $PSNativeCommandUseErrorActionPreference
# is enabled - all documented invocation paths here use powershell.exe 5.1.
#
# [P2-15] 退出条件 + 依赖前置检查：
#   * 旧脚本 while($true) 无任何退出条件，且依赖的 rbfeatctl.exe 本包内不存在
#     （需由 ctl 构建产出）——双击即陷入永久循环 / 无限重复报错。
#   * 现：exe 缺失立即失败退出；-MaxMinutes / -MaxShrinks 提供有界运行。
param(
    [int]$MaxMinutes = 0,   # 0 = 不按时间退出（仍受 MaxShrinks 约束）
    [int]$MaxShrinks = 0    # 0 = 不按次数退出
)
$ErrorActionPreference = 'Stop'

$exe = Join-Path $PSScriptRoot 'rbfeatctl.exe'
if (-not (Test-Path $exe)) {
    Write-Error "rbfeatctl.exe not found next to this script: $exe  (build ctl/rbfeatctl first)"
    exit 1
}

$deadline = if ($MaxMinutes -gt 0) { (Get-Date).AddMinutes($MaxMinutes) } else { $null }
$shrinks  = 0
while ($true) {
    if ($deadline -and (Get-Date) -gt $deadline) {
        Write-Host "[rbmemguard] time limit ($MaxMinutes min) reached, exiting."
        break
    }
    if ($MaxShrinks -gt 0 -and $shrinks -ge $MaxShrinks) {
        Write-Host "[rbmemguard] shrink limit ($MaxShrinks) reached, exiting."
        break
    }
    $os = Get-CimInstance Win32_OperatingSystem
    if ($os.FreePhysicalMemory / 1KB -lt 8192) {
        & $exe shrink 15
        $shrinks++
    }
    Start-Sleep 5
}
