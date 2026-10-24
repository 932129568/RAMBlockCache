# run_diskspd.ps1 —— diskspd 冒烟：非缓存基线 -> attach F/D -> 全类型 -> detach 校验脏=0
# 前置：驱动已安装并 attach；diskspd.exe 在 PATH 或 -Diskspd 路径。
# 错误处理约定（规格书 D.3）：$ErrorActionPreference='Stop'，失败不静默、以非零码退出。
# 此行与 rb_common.ps1 内的同名设置等价；显式声明保证公共库缺失时也 fail-closed。
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rb_common.ps1')
Assert-Admin

$ctl     = Join-Path $ProjectRoot 'ctl\ramboostctl.exe'   # 若已编译
$diskspd = 'diskspd.exe'
$work    = 'F:\rbsmoke.bin'

# [FIX S-06] Run-Diskspd 参数化：附加参数数组 splat，写比例由调用方显式传入。
# 旧代码把 -replace 作用在文件路径上（本意是从参数串去掉 -w70 写比例），
# 结果 "CACHED random read-only" 一轮实为与 70/30 完全相同的混合压测——
# 基准标签造假，结论不可信。read-only 显式 -w0（0% 写 = 纯读）。
function Run-Diskspd($label, $path, [string[]]$ExtraArgs = @()) {
    Write-Host "`n===== $label ====="
    $dargs = @('-c2G', '-t4', '-o8', '-b64K', '-Suw', '-d10') + $ExtraArgs + @($path)
    & $diskspd @dargs
}

# 1) 基线：未缓存（驱动 attach 前 / 先 detach）
Write-Host '[step1] 非缓存基线（先 detach）...'
if (Test-Path $ctl) { & $ctl detach F; & $ctl detach D }
Run-Diskspd 'BASELINE (uncached)' $work @('-w70')

# 2) 热 attach F/D（免重启，立即生效）
Write-Host '[step2] attach F/D ...'
if (Test-Path $ctl) { & $ctl attach F; & $ctl attach D } else { Write-Host '  (ctl 未编译：请用 WPF/ctl 对 F,D 执行 ATTACH_VOLUME)' }

# 3) 全类型压测（读/写/混合）
Run-Diskspd 'CACHED random 70/30' $work @('-w70')
# [FIX S-06] read-only 显式 -w0：纯读压测，不再借道路径字符串替换。
Run-Diskspd 'CACHED random read-only' $work @('-w0')

# 4) detach 并校验脏=0（回写型卷 detach 前驱动已 flush）
Write-Host '[step4] detach F/D ...'
if (Test-Path $ctl) {
    & $ctl detach F
    & $ctl detach D
    Write-Host '[check] GET_STATS -> DirtyBytes 应为 0（detach 已回写）'
    & $ctl status
}
Write-Host '冒烟结束。'
Read-Host '按回车关闭'
