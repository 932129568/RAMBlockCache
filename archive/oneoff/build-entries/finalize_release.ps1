# finalize_release.ps1 -- P0.2/P0.3 发布收尾（必须提权执行）
# 2026-11-06. 一次 UAC 会话完成：
#   1) dist\RAMBlockCache.sys 哈希 == 245c6681 锚点 + 签名 Valid（门禁前置）
#   2) 把新打的 RAMBlockCache-Setup.exe 复制到 ASCII 路径暂存（规避中文路径
#      传给 signtool 的不确定性），signtool /sm /s My 机器证书签名
#   3) 校验 Setup 签名 Valid，记录 sys + setup 双 SHA256
#   4) boot_guard.ps1 / logon_disarm.ps1 落 C:\ProgramData\RAMBoost
#   5) 注册 RAMBoostBootGuard(ONSTART) / RAMBoostLogonDisarm(ONLOGON)
#      计划任务（SYSTEM / HIGHEST）——开发机与 P1.5 分发链看齐
#   6) 签名后回写仓库 dist\ + 刷新 rb-deploy 暂存
# 幂等；每步日志落 C:\ProgramData\rb-deploy\finalize-1106.log。
# 用法：powershell -ExecutionPolicy Bypass -File src\scripts\finalize_release.ps1
param(
    [string]$SetupPath = "",
    [string]$SysPath   = ""
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $SetupPath) { $SetupPath = Join-Path $root 'dist\RAMBlockCache-Setup.exe' }
if (-not $SysPath)   { $SysPath   = Join-Path $root 'dist\RAMBlockCache.sys' }

$logDir  = 'C:\ProgramData\rb-deploy'
$log     = Join-Path $logDir 'finalize-1106.log'
$stage   = Join-Path $logDir 'RAMBlockCache-Setup.exe'
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'
# 哈希销动态解析（一次破锚点批后 sys 换新）：优先 dist\<sys>.sha256 sidecar
# （finalize_batch.ps1 写），否则从 deploy.ps1 的 DefaultExpectedSha256 行读
# （sync_anchors.ps1 维护三处同步）。不再硬编码任何一代哈希。
$anchor = ''
$sidecar = "$SysPath.sha256"
if (Test-Path -LiteralPath $sidecar) {
    $first = Get-Content -LiteralPath $sidecar -TotalCount 1
    if ($first -match '(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])') { $anchor = $matches[0].ToLower() }
}
if (-not $anchor) {
    $dep = Get-Content -LiteralPath (Join-Path $root 'src\scripts\deploy.ps1') -Raw
    $mm = [regex]::Match($dep, "(?m)DefaultExpectedSha256\s*=\s*'([0-9a-fA-F]{64})'")
    if ($mm.Success) { $anchor = $mm.Groups[1].Value.ToLower() }
}
if (-not $anchor) { Write-Host '[X] no sys hash anchor (sidecar/deploy.ps1) - refusing to sign'; exit 1 }

function L([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m)
}

# --- 0. 前置检查 ---
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated'
    exit 1
}
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
'' | Set-Content -LiteralPath $log
if (-not (Test-Path -LiteralPath $SetupPath)) { L "MISSING setup: $SetupPath"; exit 1 }
if (-not (Test-Path -LiteralPath $SysPath))   { L "MISSING sys: $SysPath"; exit 1 }
if (-not (Test-Path -LiteralPath $sigtool))  { L "MISSING signtool: $sigtool"; exit 1 }
L ("setup: " + $SetupPath)
L ("sys  : " + $SysPath)

# --- 1. sys 哈希与签名门禁（红线条：不满足绝不继续） ---
$sysHash = (Get-FileHash -LiteralPath $SysPath -Algorithm SHA256).Hash.ToLower()
if ($sysHash -ne $anchor) { L ("SYS HASH MISMATCH got=" + $sysHash + " expected=" + $anchor); exit 1 }
$sysSig = Get-AuthenticodeSignature -LiteralPath $SysPath
if ($sysSig.Status -ne 'Valid') { L ("SYS SIGNATURE " + $sysSig.Status); exit 1 }
L ("[1] sys OK: hash=" + $sysHash.Substring(0,8) + " sig=" + $sysSig.Status)

# --- 2. 暂存 + 签名 Setup（在 ASCII 路径上执行签名） ---
Copy-Item -LiteralPath $SetupPath -Destination $stage -Force
L "[2] signing Setup exe (305MB, may take a moment)..."
& $sigtool sign /sm /s My /sha1 $thumb /fd SHA256 $stage 2>&1 | ForEach-Object { L ("  signtool: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("SIGN FAILED exit=" + $LASTEXITCODE); exit 1 }

# --- 3. 校验 + 双哈希记录 ---
$setupSig = Get-AuthenticodeSignature -LiteralPath $stage
if ($setupSig.Status -ne 'Valid') { L ("SETUP SIGNATURE " + $setupSig.Status); exit 1 }
$setupHash = (Get-FileHash -LiteralPath $stage -Algorithm SHA256).Hash.ToLower()
L ("[3] setup OK: sig=" + $setupSig.Status + " sha256=" + $setupHash)

# --- 4. 看门脚本落 ProgramData\RAMBoost ---
$pd = 'C:\ProgramData\RAMBoost'
if (-not (Test-Path -LiteralPath $pd)) { New-Item -ItemType Directory -Path $pd -Force | Out-Null }
Copy-Item (Join-Path $root 'src\scripts\boot_guard.ps1')   (Join-Path $pd 'boot_guard.ps1')   -Force
Copy-Item (Join-Path $root 'src\scripts\logon_disarm.ps1') (Join-Path $pd 'logon_disarm.ps1') -Force
L ("[4] guard scripts staged in " + $pd)

# --- 5. 注册看门计划任务（幂等 /F；与安装引擎同参数面） ---
$bg = Join-Path $pd 'boot_guard.ps1'
$ld = Join-Path $pd 'logon_disarm.ps1'
$tr1 = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $bg + '"'
$tr2 = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $ld + '"'
& schtasks.exe /Create /F /TN RAMBoostBootGuard /SC ONSTART /RU SYSTEM /RL HIGHEST /TR $tr1 2>&1 | ForEach-Object { L ("  schtasks: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("TASK RAMBoostBootGuard FAILED exit=" + $LASTEXITCODE); exit 1 }
& schtasks.exe /Create /F /TN RAMBoostLogonDisarm /SC ONLOGON /RU SYSTEM /RL HIGHEST /TR $tr2 2>&1 | ForEach-Object { L ("  schtasks: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("TASK RAMBoostLogonDisarm FAILED exit=" + $LASTEXITCODE); exit 1 }
L "[5] guard tasks registered (SYSTEM/HIGHEST)"

# --- 6. 签名后回写仓库 dist + 刷新暂存 sys ---
Copy-Item -LiteralPath $stage -Destination $SetupPath -Force
Copy-Item -LiteralPath $SysPath -Destination (Join-Path $logDir 'RAMBlockCache.sys') -Force
$verifyBack = Get-AuthenticodeSignature -LiteralPath $SetupPath
L ("[6] repo dist refreshed: sig=" + $verifyBack.Status)
if ($verifyBack.Status -ne 'Valid') { L "BACK-COPY SIGNATURE INVALID"; exit 1 }

L "ALL OK"
Write-Host '=== finalize OK ==='
exit 0
