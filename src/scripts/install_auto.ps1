# [BUG-002] 本脚本已废弃 —— 旧版会把过滤驱动经错误路径引入引导卷（0x7B 事故根源），
# 且使用非标准卷类 GUID（BUG-009）。为防误用，此脚本一律拒绝执行。
Write-Host "[X] install_auto.ps1 已废弃。唯一安装入口：" -ForegroundColor Red
Write-Host "    setup\ramboostsetup.exe  （标准卷类 UpperFilters，volsnap 保留）"
Write-Host "    卸载：scripts\uninstall.ps1"
exit 1
