# devloop.ps1 - RAMBlockCache 开发循环一键脚本（U0-1，2026-10-19）
# Why: 测试循环原为六步全手工（编译 -> signtool -> copy -> sync_anchors ->
#   deploy -> 手动重启 -> 挂载），每轮 >=10 分钟 + 一次重启。本脚本收敛为一条命令：
#     构建 -> （签名 -> 锚点同步）-> 部署（测试模式零重启 / 生产模式一次重启）
# 纪律（与 iteration-plan / install-ux 方案一致）：
#   - 默认走生产模式：deploy.ps1 自带验签 + 锚点 + boot_guard，如实提示需重启一次；
#   - -TestMode 走零重启 dev loop，前提是安装器曾以 /TestMode 安装
#     （DEMAND_START 服务 + 注册表 TestMode=1），否则 fail-fast 并给出指引；
#   - -NoSign 产物仅本机功能验证，禁止部署（未签名驱动进引导路径 = 0x7B 事故形态）；
#   - 运行时挂栈（U0-1 驱动路径）默认关：仅 TestMode 安装写入注册表开关后才可达；
#     挂栈位置与 AddDevice 不同（栈顶 vs PDO 上），VSS/热插拔待 W0 Verifier 矩阵。
param(
    [switch]$NoSign,
    [switch]$TestMode,
    [string]$Configuration = "Release",
    [string[]]$AttachLetters = @()
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sys  = Join-Path $root "src\driver\x64\Release\RAMBlockCache.sys"
$ctl  = Join-Path $root "src\ctl\ramboostctl.exe"
$svc  = "RAMBlockCache"
$thumb = "DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431"   # CN=RAMBoostFS Root（与 sign_all.cmd 同值）

function Show-Step([int]$n, [string]$msg) {
    Write-Host ""
    Write-Host "== [$n] $msg ==" -ForegroundColor Cyan
}

# ---------- 0) 前提：管理员（attach IOCTL / 服务控制 / System32 写入都要） ----------
. (Join-Path $PSScriptRoot "rb_common.ps1")
Assert-Admin
if (-not (Test-Path -LiteralPath $ctl)) { throw "ramboostctl.exe not found: $ctl (build ctl first)" }

# ---------- 1) 构建驱动（cl/link 直编；输出未签名 sys） ----------
Show-Step 1 "building driver ($Configuration) ..."
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "src\driver\_build_manual.ps1")
if ($LASTEXITCODE -ne 0) { throw "driver build failed (exit $LASTEXITCODE)" }
if (-not (Test-Path -LiteralPath $sys)) { throw "build produced no sys: $sys" }
$hash = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash
Write-Host "  sys    : $sys"
Write-Host "  sha256 : $hash"

# ---------- 2) 签名 + 锚点同步（-NoSign 跳过并高亮警告） ----------
if ($NoSign) {
    Write-Host ""
    Write-Host "[!!] -NoSign: UNSIGNED build - local functional verification ONLY." -ForegroundColor Yellow
    Write-Host "     DO NOT DEPLOY. Unsigned driver in the boot path = 0x7B bootloop" -ForegroundColor Yellow
    Write-Host "     (incident-0x7b-bootloop-2026-10-17). TestMode sc start also needs" -ForegroundColor Yellow
    Write-Host "     testsigning ON (bcdedit) or Code Integrity will refuse to load." -ForegroundColor Yellow
} else {
    Show-Step 2 "signing (user cert, /sm) + syncing deploy anchors ..."
    $signTool = Find-SignTool
    if (-not $signTool) { throw "signtool.exe not found - set SIGTOOL=<path> (see rb_common.ps1 Find-SignTool)" }
    & $signTool sign /sm /s My /sha1 $thumb /fd SHA256 $sys
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed (exit $LASTEXITCODE). NOTE: signing needs an ELEVATED window (LM\My private key is not visible unelevated)."
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "sync_anchors.ps1")
    if ($LASTEXITCODE -ne 0) { throw "sync_anchors failed (exit $LASTEXITCODE)" }
}

# ---------- 3) 部署 ----------
if ($TestMode) {
    # --- 3a) 测试模式：DEMAND_START + 运行时挂栈，零重启 ---
    Show-Step 3 "TestMode deploy (zero-reboot dev loop) ..."
    $s = Get-Service -Name $svc -ErrorAction SilentlyContinue
    if (-not $s) {
        throw "service '$svc' not found. Install first (elevated): src\setup\ramboostsetup.exe /TestMode"
    }
    if ($s.StartType -ne "Manual") {
        throw "service start type = $($s.StartType); TestMode requires DEMAND_START (shows as Manual). Reinstall: ramboostsetup.exe /TestMode"
    }
    $tm = (Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters" -Name TestMode -ErrorAction SilentlyContinue).TestMode
    if ($tm -ne 1) {
        throw "registry TestMode != 1 (runtime attach path gated off). Install with: ramboostsetup.exe /TestMode"
    }

    # 停服 = 干净卸载（DriverUnload: 全量回写 -> 停线程 -> 逐卷拆除）；
    # join 超时时驱动按设计中止卸载保数据 -> 必须重启一次，不得假装成功。
    if ($s.Status -ne "Stopped") {
        Stop-Service -Name $svc -Force -ErrorAction Stop
        $w = 0
        while ((Get-Service -Name $svc).Status -ne "Stopped" -and $w -lt 30) {
            Start-Sleep -Milliseconds 500
            $w++
        }
        if ((Get-Service -Name $svc).Status -ne "Stopped") {
            throw "sc stop timed out (~15s): driver aborted unload by design (data safety). REBOOT ONCE, then re-run devloop -TestMode."
        }
        Write-Host "  [ok] service stopped (dirty flushed by DriverUnload)"
    }

    $dest = Join-Path $env:SystemRoot "System32\drivers\RAMBlockCache.sys"
    Copy-Item -LiteralPath $sys -Destination $dest -Force
    Write-Host "  [ok] sys swapped: $dest"
    Start-Service -Name $svc -ErrorAction Stop
    Write-Host "  [ok] service started (DEMAND_START; runtime attach path armed)"

    # attach：默认全部非系统固定卷（与"开机自动挂载只挂数据卷"同口径）
    if ($AttachLetters.Count -eq 0) {
        $sysRoot = [IO.Path]::GetPathRoot([Environment]::GetFolderPath("System")).TrimEnd('\')
        $AttachLetters = @(Get-CimInstance Win32_LogicalDisk -Filter "DriveType=3" |
            Where-Object { $_.DeviceID -ne $sysRoot } |
            ForEach-Object { $_.DeviceID.TrimEnd(':') })
    }
    foreach ($l in $AttachLetters) {
        Write-Host "  [..] ramboostctl attach $l ..."
        & $ctl attach $l
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  [!!] attach $l failed (exit $LASTEXITCODE)." -ForegroundColor Yellow
            Write-Host "       If this build predates the U0-1 runtime-attach path, TestMode attach" -ForegroundColor Yellow
            Write-Host "       is unavailable: use production mode (deploy + reboot) instead." -ForegroundColor Yellow
        }
    }
    & $ctl volumes

    Write-Host ""
    Write-Host "== acceptance checklist (W0-2 subset; mark manually) ==" -ForegroundColor Green
    Write-Host "  [ ] volumes: every data volume shows attached=1"
    Write-Host "  [ ] stats   : two-pass read of a test file -> ReadHit grows (ramboostctl status)"
    Write-Host "  [ ] verifier: zero new warnings under 'verifier /standard /driver RAMBlockCache.sys'"
    Write-Host "  [ ] teardown: ramboostctl detach <X> removes the filter device (no leftover !devobj)"
} else {
    # --- 3b) 生产模式：deploy.ps1（验签 + 锚点 + boot_guard），一次重启不可协商 ---
    Show-Step 3 "production deploy (deploy.ps1: verify + anchor + boot_guard) ..."
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "deploy.ps1")
    if ($LASTEXITCODE -ne 0) { throw "deploy.ps1 failed (exit $LASTEXITCODE)" }
    Write-Host ""
    Write-Host "== production mode: REBOOT ONCE (BOOT_START image swap) ==" -ForegroundColor Yellow
    Write-Host "   category-inherent cost (PrimoCache identical) - do NOT promise no-reboot install."
    Write-Host "   after reboot: powershell -File src\scripts\verify_install.ps1   (expect 13 pass, 0 fail)"
}

Write-Host ""
Write-Host "=== devloop done (sys sha256=$hash) ===" -ForegroundColor Green
