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
Write-Host ''
Write-Host '  [1] ON  (开启测试签名 = 允许加载 RAMBlockCache 自签驱动，需重启生效)'
Write-Host '  [2] OFF (关闭测试签名 = 玩 Valorant 前必做，需重启生效)'
$c = Read-Host '选 1 或 2'
if ($c -eq '1') {
    bcdedit /set testsigning on
    Write-Host '[ok] testsigning 已设为 ON。请重启后再加载/使用 RAMBlockCache。'
} else {
    bcdedit /set testsigning off
    Write-Host '[ok] testsigning 已设为 OFF。请重启后再启动 Valorant。'
}
Read-Host '按回车关闭'
