# =============================================================================
#  RB-finalize-172.ps1  —  一键盘：签名 + 同步锚点 + 部署（不重启）
#
#  为什么需要这一层：`RB-sign-deploy-150.ps1` 里有一个硬编码的
#  `-ExpectSha = 'a49fad95…'`（当时被审阅过的那次构建）。本轮 172 号内核改动
#  把驱动重编成了新哈希，直接跑旧脚本会在 Step 0 就 `artifact drifted from the
#  reviewed build` 中止——那是 pin 机制在正常工作，不是故障。
#
#  本脚本不绕过它：期望值改从 `expected-sys-sha.txt` 读，并且**必须与实际产物
#  逐字节相符**才继续。二者不符 = 有人在你签名之前又改了源码/重建了驱动，
#  此时应该停下来重新审阅，而不是把新哈希写进文件继续。所以文件名里的 172
#  是"这次审阅对应的批次号"，不是"随便签"。
#
#  用法：双击 `RB-finalize-172.cmd`（它会要求管理员权限）。
#  日志：%TEMP%\rb-sign-deploy-150.log     结果：%TEMP%\rb-sign-deploy-150.result
#  不重启。重启是单独的决定。
# =============================================================================
$ErrorActionPreference = 'Continue'

function Die([string]$msg) {
    Write-Host ''
    Write-Host "*** ABORT: $msg ***" -ForegroundColor Red
    Write-Host ''
    exit 4
}

$Root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $Root 'build\build.ps1'))) {
    # 兼容脚本被放在仓库根时的情况
    $Root = $PSScriptRoot
}
$Artifact = Join-Path $Root 'src\driver\x64\Release\RAMBlockCache.sys'
$ExpectFile = Join-Path $PSScriptRoot 'expected-sys-sha.txt'
$Signer = Join-Path $PSScriptRoot 'RB-sign-deploy-150.ps1'

if (-not (Test-Path -LiteralPath $Artifact)) { Die "驱动产物不存在：$Artifact（先跑 build\build.ps1 -Target driver）" }
if (-not (Test-Path -LiteralPath $ExpectFile)) { Die "缺少期望哈希文件：$ExpectFile" }
if (-not (Test-Path -LiteralPath $Signer)) { Die "缺少签名脚本：$Signer" }

$expect = ((Get-Content -LiteralPath $ExpectFile -TotalCount 1) -replace '\s', '').ToLower()
if ($expect -notmatch '^[0-9a-f]{64}$') { Die "expected-sys-sha.txt 首行不是 64 位十六进制：'$expect'" }

$actual = (Get-FileHash -LiteralPath $Artifact -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item -LiteralPath $Artifact).Length

Write-Host ''
Write-Host '  artifact : src\driver\x64\Release\RAMBlockCache.sys'
Write-Host "  size     : $size bytes"
Write-Host "  expected : $expect"
Write-Host "  actual   : $actual"
Write-Host ''

if ($actual -ne $expect) {
    Write-Host '*** 产物与已审阅的构建不一致 ***' -ForegroundColor Red
    Write-Host '  说明源码在你签名之前又被改过或重建过。' -ForegroundColor Red
    Write-Host '  正确做法：停下来重新审阅这批改动，确认后再更新 expected-sys-sha.txt。' -ForegroundColor Red
    Write-Host '  不要为了让脚本跑通而直接改期望值。' -ForegroundColor Red
    exit 4
}

Write-Host '  [ok] 产物与已审阅构建一致，交棒给 RB-sign-deploy-150.ps1（签名 / 锚点 / 部署）' -ForegroundColor Green
Write-Host ''

& $Signer -ExpectSha $expect
$rc = $LASTEXITCODE
if ($null -eq $rc) { $rc = 0 }

Write-Host ''
if ($rc -ne 0) {
    Write-Host "*** 签名/部署链失败（exit=$rc），未重启。看上面日志定位。 ***" -ForegroundColor Red
    exit $rc
}

# ---- finalize marker（重启后验收用）----------------------------------------
# `src/scripts/verify_final_reboot.ps1` 的 X2 gate 要求：证明"重启发生在收尾之后"。
# 它读 C:\ProgramData\rb-deploy\finalize-utc.txt，判据是 24 字符定宽 ISO-8601 UTC
# （定宽 ⇒ 字典序 == 时序，脚本 :76 直接做字符串比较）。格式写错就静默 FAIL。
# 时点选择：签名+部署成功之后。G10 的教训仍成立——重启后**不要**再双击本脚本，
# 否则 marker 被覆盖成更晚的时刻，`boot_utc > marker` 会变成假（假 FAIL，非假 PASS）。
$MarkerDir = 'C:\ProgramData\rb-deploy'
$MarkerPath = Join-Path $MarkerDir 'finalize-utc.txt'
try {
    if (-not (Test-Path -LiteralPath $MarkerDir)) {
        New-Item -ItemType Directory -Path $MarkerDir -Force | Out-Null
    }
    $markerUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture)
    [IO.File]::WriteAllText($MarkerPath, $markerUtc, [System.Text.UTF8Encoding]::new($false))
    Write-Host ('  [ok] finalize marker = ' + $markerUtc) -ForegroundColor Green
    Write-Host ('       ' + $MarkerPath) -ForegroundColor Green
} catch {
    Write-Host ('  [WARN] marker 写入失败：' + $_.Exception.Message) -ForegroundColor Yellow
    Write-Host '         重启后 verify_final_reboot.ps1 的 X2 gate 会 FAIL（可手工补写同格式文件）' -ForegroundColor Yellow
}

Write-Host '============================================================================' -ForegroundColor Green
Write-Host ' 完成：新驱动已签名、锚点已同步、已部署到 System32。' -ForegroundColor Green
Write-Host '' -ForegroundColor Green
Write-Host ' 下一步（必须）：重启本机。' -ForegroundColor Yellow
Write-Host '   驱动是 BOOT_START + UpperFilters，只有重启才会加载新版本。' -ForegroundColor Yellow
Write-Host '   部署脚本已写 DeployAck，重启后由驱动自检握手。' -ForegroundColor Yellow
Write-Host '' -ForegroundColor Yellow
Write-Host ' 重启后要核的三件事（告诉我结果，我来跑只读验收）：' -ForegroundColor Yellow
Write-Host '   1) sc query 看驱动 State=RUNNING；System32\drivers\RAMBlockCache.sys 哈希 == dist 哈希' -ForegroundColor Yellow
Write-Host '   2) 事件日志 / 驱动日志无 [300] self-disable' -ForegroundColor Yellow
Write-Host '   3) L2 仍应是关闭状态（Features\Flags 的 L2 位未开）——本次改动不改变默认姿态' -ForegroundColor Yellow
Write-Host '============================================================================' -ForegroundColor Green
exit 0
