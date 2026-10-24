# game_mode.ps1 —— testsigning on/off 切换（自签驱动需要 testsigning on）
# 玩法：
#   玩 Valorant 前：执行 off -> 重启（Valorant 反作弊会因 testsigning 报错）
#   不玩、要跑 RAMBlockCache：执行 on -> 重启
# 错误处理约定（规格书 D.3）：$ErrorActionPreference='Stop'，失败不静默、以非零码退出。
# 此行与 rb_common.ps1 内的同名设置等价；显式声明保证公共库缺失时也 fail-closed。
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rb_common.ps1')
Assert-Admin

Write-Host "当前 testsigning 状态："
bcdedit /enum | Select-String 'testsigning'
# [FIX S-07] /enum 查退出码：bcdedit 不可用时状态不可见，fail-closed 退出 2。
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] bcdedit /enum failed (exit $LASTEXITCODE); cannot read testsigning state." -ForegroundColor Red
    Read-Host '按回车关闭'
    exit 2
}
Write-Host ''
Write-Host '  [1] ON  (开启测试签名 = 允许加载 RAMBlockCache 自签驱动，需重启生效)'
Write-Host '  [2] OFF (关闭测试签名 = 玩 Valorant 前必做，需重启生效)'
Write-Host '  [Q] 退出（不做任何更改）'
# [FIX S-07] 输入循环仅收 1/2/Q：空/其他重新提示——旧代码任何非 '1' 输入
# （含空回车/误敲）即 bcdedit /set testsigning off 无确认，误操作+重启后
# 驱动不加载，表现为"产品坏了"。
$c = ''
while ($true) {
    $c = Read-Host '选 1 / 2 / Q(退出)'
    if ($c -eq '1' -or $c -eq '2' -or $c -eq 'Q' -or $c -eq 'q') { break }
    Write-Host '  输入无效，请选 1、2 或 Q。'
}
if ($c -eq 'Q' -or $c -eq 'q') {
    Write-Host '[..] 已取消，未做任何更改。'
    Read-Host '按回车关闭'
    exit 0
}
# [FIX S-07] OFF 需显式确认串：testsigning off 后自签驱动不加载，不可误触。
if ($c -eq '2') {
    $confirm = Read-Host '确认关闭 testsigning？关闭后 RAMBlockCache 自签驱动将不加载（玩 Valorant 前必做）。输入 YES 确认'
    if ($confirm -ne 'YES') {
        Write-Host '[..] 未确认，已取消，未做任何更改。'
        Read-Host '按回车关闭'
        exit 0
    }
}
if ($c -eq '1') {
    bcdedit /set testsigning on
    # [FIX S-07] /set 查退出码：失败非零收尾（3），说清原因。
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[X] bcdedit /set testsigning on failed (exit $LASTEXITCODE)." -ForegroundColor Red
        Read-Host '按回车关闭'
        exit 3
    }
    Write-Host '[ok] testsigning 已设为 ON。请重启后再加载/使用 RAMBlockCache。'
} else {
    bcdedit /set testsigning off
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[X] bcdedit /set testsigning off failed (exit $LASTEXITCODE)." -ForegroundColor Red
        Read-Host '按回车关闭'
        exit 3
    }
    Write-Host '[ok] testsigning 已设为 OFF。请重启后再启动 Valorant。'
}
Read-Host '按回车关闭'
exit 0
