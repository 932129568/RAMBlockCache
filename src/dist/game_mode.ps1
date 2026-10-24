# game_mode.ps1 - [FIX round8] 转发垫片（原为旧缺陷副本，已退役）
#
# 本文件原是 round3 #11 修复前的旧副本：任何非 '1' 输入即关 testsigning（无确认、
# 无退出码检查）——经同目录 game_mode.cmd 双击可执行，属"带着已知缺陷可执行"。
# round3 #11/[FIX S-07] 已在权威副本 src\scripts\game_mode.ps1 修复（输入循环 +
# YES 确认 + bcdedit 退出码检查）。游离件整体移出包外需用户授权（iteration-prd
# 未决项 1），在授权前本垫片保证"任何执行都落到修复版"，零缺陷路径可达。
#
# 用法（等价于权威版）：powershell -ExecutionPolicy Bypass -File <本文件>
$auth = Join-Path $PSScriptRoot '..\scripts\game_mode.ps1'
if (-not (Test-Path -LiteralPath $auth)) {
    Write-Host "[X] 权威脚本缺失：$auth" -ForegroundColor Red
    Write-Host "    请从源码包 src\scripts\game_mode.ps1 运行，或改用正式交付包入口。"
    exit 1
}
& $auth @args
exit $LASTEXITCODE
