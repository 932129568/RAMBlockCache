# deploy_w15.ps1 -- W1.5 新驱动 签名→入 dist→锚点→部署 一条命令（需提权窗运行）
# Why: W1.5（64MB 大块 IO 门 + 读分片直读 + 写分块 + 同步 IO 池化）不部署则运行时仍是
#   旧 8MB 门驱动——回写/预取/无限周期全部被门挡在缓存外（16MB 顺序块直通硬盘）。
#   本脚本把"签名→复制→sync_anchors→门禁→部署"收敛为一次提权运行。
# Usage (elevated PowerShell):
#   powershell -ExecutionPolicy Bypass -File src\scripts\deploy_w15.ps1              # 生产模式（部署后重启一次）
#   powershell -ExecutionPolicy Bypass -File src\scripts\deploy_w15.ps1 -TestMode   # 测试模式（DEMAND_START，零重启）
# Discipline: every step checks exit code, fail fast. Pure ASCII.
# [FIX QA5-UI5] param() 声明必须置于任何语句之前（含 $ErrorActionPreference 赋值）：
# 原文件无 param()，按文档用法加 -TestMode 直接参数绑定报错脚本不执行；
# 不加则 $TestMode 恒 $null → 静默走生产模式（与"测试模式免重启"预期相反）。
param([switch]$TestMode)
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot "rb_common.ps1")
Assert-Admin

$sys     = Join-Path $root "src\driver\x64\Release\RAMBlockCache.sys"
$dist    = Join-Path $root "dist\RAMBlockCache.sys"
$thumb   = "DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431"   # CN=RAMBoostFS Root（sign_all.cmd 同值）
$testMode = [bool]$TestMode

if (-not (Test-Path -LiteralPath $sys)) { throw "W1.5 build not found: $sys (run src\driver\_build_manual.ps1 first)" }
$pre = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash
Write-Host "== [1/5] sign W1.5 build =="
Write-Host "  before: $pre (unsigned)"
$signTool = Find-SignTool
if (-not $signTool) { throw "signtool.exe not found - set SIGTOOL=<path> (see rb_common.ps1 Find-SignTool)" }
& $signTool sign /sm /s My /sha1 $thumb /fd SHA256 $sys
if ($LASTEXITCODE -ne 0) { throw "signtool failed (exit=$LASTEXITCODE) - run this script in an ELEVATED window (private key lives in LM\My)" }
$sig = Get-AuthenticodeSignature -LiteralPath $sys
if ($sig.Status -ne "Valid") { throw "post-sign status = $($sig.Status) - refusing to deploy (fail-closed)" }
Write-Host "  after : $((Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash) ($($sig.Status))"

Write-Host "== [2/5] copy to dist =="
Copy-Item -LiteralPath $sys -Destination $dist -Force

Write-Host "== [3/5] sync deploy anchors (fail-closed: unsigned refused) =="
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "sync_anchors.ps1")
if ($LASTEXITCODE -ne 0) { throw "sync_anchors failed (exit=$LASTEXITCODE)" }

Write-Host "== [4/5] signature gate =="
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "check_signature.ps1")
if ($LASTEXITCODE -ne 0) { throw "check_signature FAILED (exit=$LASTEXITCODE) - anchors/signature out of sync" }

Write-Host "== [5/5] deploy =="
if ($testMode) {
    # 测试模式：C 引擎切 DEMAND_START + TestMode=1 + 不写 UpperFilters（自身带验签预检；
    #   ramboostsetup.exe /TestMode——勿用 ramboostsetup.ps1，那是旧 V9 模型、无此参数）
    Copy-Item -LiteralPath $sys -Destination (Join-Path $root "src\setup\RAMBlockCache.sys") -Force
    & (Join-Path $root "src\setup\ramboostsetup.exe") /TestMode
    if ($LASTEXITCODE -ne 0) { throw "TestMode install failed (exit=$LASTEXITCODE)" }
    Write-Host "=== DEPLOY OK (TestMode, ZERO reboot) ==="
    Write-Host "next: sc start RAMBlockCache ; ramboostctl attach D: (then E:/F:)"
} else {
    # 生产模式：deploy.ps1（验签+锚点+boot_guard，MoveFileEx 排队替换）→ 重启一次
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "deploy.ps1")
    if ($LASTEXITCODE -ne 0) { throw "deploy.ps1 failed (exit=$LASTEXITCODE)" }
    Write-Host "=== DEPLOY OK (production) ==="
    Write-Host "next: REBOOT ONCE (BOOT_START image swap). After reboot volumes auto-attach (AutoAttach=1)."
}
Write-Host "NOTE: per-volume write-back is runtime state (not persisted) - set it again after reboot/mount if needed."
