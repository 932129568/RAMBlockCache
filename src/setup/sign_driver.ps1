# Reusable driver signing helper (run elevated). Signs with the local RAMBoostFS root.
# [FIX S-10] 对齐 rb_common.ps1 的发现逻辑与时间戳策略（dot-source 复用，不复制）：
#   signtool 解析 = 显式 -SignTool > RB_SIGNTOOL > Windows Kits 递归（deploy.ps1 同款）。
#   结果文件契约保持：-Target/-Thumbprint/-ResultFile 参数面与 ResultFile 输出不变。
# EAP 纪律：本文件需要 Continue（"失败也写结果文件"的既有契约）。注意赋值必须
# 写在 dot-source 之后——rb_common.ps1 内部的 $ErrorActionPreference='Stop' 会
# 泄漏进调用方作用域（2026-10-19 实测），写在前面会被覆盖回 Stop。
param(
    [Parameter(Mandatory=$true)][string]$Target,
    [string]$Thumbprint = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431',
    [string]$ResultFile = '',
    [string]$SignTool = '',
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$NoTimestamp
)
. (Join-Path $PSScriptRoot '..\scripts\rb_common.ps1') -SignTool $SignTool
$ErrorActionPreference = 'Continue'
$st = $Script:SignTool
$lines = @()
if (-not $st) {
    $lines += "SIGNTOOL NOT FOUND: set -SignTool or RB_SIGNTOOL, or install Windows Kits (deploy.ps1-style Kits scan found nothing)"
}
elseif (-not (Test-Path $Target)) {
    $lines += "TARGET NOT FOUND: $Target"
}
else {
    $tsArgs = if ($NoTimestamp) { @() } else { @('/t', $TimestampUrl) }
    $lines += & $st sign /sm /s My /sha1 $Thumbprint /fd SHA256 @tsArgs $Target 2>&1
    $lines += "SIGN_EXIT=$LASTEXITCODE"
}
$out = if ($ResultFile) { $ResultFile } else { Join-Path $env:TEMP 'rbsign_result.txt' }
$lines | Set-Content -Encoding UTF8 -Path $out
