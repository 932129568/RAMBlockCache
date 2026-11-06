# finalize_batch.ps1 -- 提权轮次 A：新驱动签名 -> dist -> 锚点 sidecar -> deploy.ps1 部署
# 2026-11-06 一次破锚点功能批；由 RB-batch-A.cmd 以管理员拉起。
# 纪律：dist\ 里只允许出现【已签名】驱动；未签名构建产物只走 ASCII 暂存路径。
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated'
    exit 1
}
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src     = Join-Path $root 'src\driver\x64\Release\RAMBlockCache.sys'
$dist    = Join-Path $root 'dist\RAMBlockCache.sys'
$stage   = 'C:\ProgramData\rb-deploy\RAMBlockCache.sys'
$sidecar = "$dist.sha256"
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'
$logDir  = 'C:\ProgramData\rb-deploy'
$log     = Join-Path $logDir 'finalize-batch.log'

function L([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m)
}

if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
'' | Set-Content -LiteralPath $log
if (-not (Test-Path -LiteralPath $src))   { L "MISSING unsigned build: $src"; exit 1 }
if (-not (Test-Path -LiteralPath $sigtool)) { L "MISSING signtool: $sigtool"; exit 1 }
$pre = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash.ToLower()
L ("[0] unsigned build: " + $pre.Substring(0,8) + " size=" + (Get-Item -LiteralPath $src).Length)

# --- 1) ASCII 暂存路径上签名 ---
Copy-Item -LiteralPath $src -Destination $stage -Force
L '[1] signing staged driver ...'
& $sigtool sign /sm /s My /sha1 $thumb /fd SHA256 $stage 2>&1 | ForEach-Object { L ("  signtool: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("SIGN FAILED exit=" + $LASTEXITCODE); exit 1 }
$s = Get-AuthenticodeSignature -LiteralPath $stage
if ($s.Status -ne 'Valid') { L ("SIGNATURE " + $s.Status); exit 1 }
$hash = (Get-FileHash -LiteralPath $stage -Algorithm SHA256).Hash.ToLower()
L ("[1] signed OK: " + $hash + " signer=" + $s.SignerCertificate.Subject)

# --- 2) 已签名驱动落 dist + sidecar（deploy.ps1 的哈希销） ---
Copy-Item -LiteralPath $stage -Destination $dist -Force
('SHA256: ' + $hash) | Set-Content -LiteralPath $sidecar -Encoding ASCII
$back = (Get-FileHash -LiteralPath $dist -Algorithm SHA256).Hash.ToLower()
if ($back -ne $hash) { L "DIST COPY HASH MISMATCH"; exit 1 }
L '[2] dist sys refreshed (signed) + sidecar written'

# --- 3) 部署：deploy.ps1（验签 fail-closed + PFRO 排队 + 武装基线 + 看门任务） ---
L '[3] running deploy.ps1 (PFRO queue expected: driver file is locked by running instance) ...'
& (Join-Path $root 'src\scripts\deploy.ps1') 2>&1 | ForEach-Object { L ("  deploy: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("DEPLOY FAILED exit=" + $LASTEXITCODE); exit 1 }
L '[3] deploy OK (System32 replacement queued for next reboot; NO reboot triggered)'

# --- 4) 哈希记录（供发布文档/锚点同步核对） ---
Add-Content -LiteralPath (Join-Path $logDir 'batch-hashes.txt') -Value ("SYS_SHA256=" + $hash)
L 'ALL OK'
Write-Host '=== batch round A OK ==='
exit 0
