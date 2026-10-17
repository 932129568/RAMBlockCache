# check_signature.ps1 -- 门禁第五件套：签名有效性 + 哈希锚点一致性
#
# 背景（0x7B 事故整改，incident-0x7b-bootloop-2026-10-17.md）：
#   门禁四件套（check_repo / check_proto_sync / refcount_model /
#   check_ps_syntax）唯独缺"签名有效性"维度——而它是唯一能砖机的维度。
#   未签名驱动被注册为 BOOT_START + UpperFilters -> CI 引导路径拒载 -> 0x7B。
#
# 本检查（只读、不依赖部署、不需要管理员）：
#   1) dist\RAMBlockCache.sys 存在
#   2) Authenticode 签名链有效（Get-AuthenticodeSignature Status == Valid：
#      链到本机任一信任根即通过——RAMBoostFS 测试根 / 未来 WHQL+EV 均可；
#      未签名被拒）
#   3) 打印签名者主题（可见性）
#   4) 文件 SHA256 与三处哈希锚点一致（deploy.ps1 / verify_deploy_signature.ps1 /
#      README.txt），且三处锚点互为一致（红线"锚点必须同步"的自动断言）
#
# 退出码：0 = PASS，1 = FAIL。输出格式与现有四件套对齐。
# 注意：本文件必须保持 UTF-8 with BOM（check_ps_syntax.ps1 自己会检查）。

[CmdletBinding()]
param(
    [string]$SysPath,
    [string]$ExpectedSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # src\scripts\.. = repo root
if (-not $SysPath) { $SysPath = Join-Path $root 'dist\RAMBlockCache.sys' }

$fail = 0
function Fail([string]$msg) { Write-Host "[FAIL] $msg"; $script:fail = 1 }
function Ok  ([string]$msg) { Write-Host "[ok] $msg" }

Write-Host '== check_signature =='
Write-Host "  sys: $SysPath"

# ---- 1) 存在性 ----
if (-not (Test-Path -LiteralPath $SysPath)) {
    Fail "dist sys not found: $SysPath"
    Write-Host 'result: FAIL (1 fail, 0 warn)'
    exit 1
}

# ---- 2) 签名有效性（链到信任根即过） ----
try {
    $sig = Get-AuthenticodeSignature -LiteralPath $SysPath -ErrorAction Stop
} catch {
    Fail "Get-AuthenticodeSignature threw: $($_.Exception.Message)"
    Write-Host 'result: FAIL (1 fail, 0 warn)'
    exit 1
}
Write-Host "  signature status: $($sig.Status)"
if ($sig.Status -ne 'Valid') {
    Fail "signature is '$($sig.Status)', expected Valid. An unsigned driver MUST NOT be deployed (BOOT_START + UpperFilters -> 0x7B). Sign it first (see README signing section)."
} else {
    Ok 'signature chain: Valid'
    # [FIX] StrictMode 下部分签名对象不暴露 SignerInfo —— try/catch 防护，取不到就跳过
    try {
        if ($sig.SignerInfo -and $sig.SignerInfo.Subject) {
            Write-Host "  signer: $($sig.SignerInfo.Subject)"
        }
    } catch { }
}

# ---- 3) 哈希与三处锚点一致性（红线） ----
$got = (Get-FileHash -LiteralPath $SysPath -Algorithm SHA256).Hash.ToLower()
Write-Host "  sha256: $got"

$anchorFiles = @(
    @{ Name = 'deploy.ps1';                  Path = (Join-Path $PSScriptRoot 'deploy.ps1') },
    @{ Name = 'verify_deploy_signature.ps1'; Path = (Join-Path $PSScriptRoot 'verify_deploy_signature.ps1') },
    @{ Name = 'README.txt';                  Path = (Join-Path $root 'README.txt') }
)

$anchors = @{}
foreach ($f in $anchorFiles) {
    if (-not (Test-Path -LiteralPath $f.Path)) { Fail "anchor file missing: $($f.Path)"; continue }
    $text = Get-Content -LiteralPath $f.Path -Raw -ErrorAction SilentlyContinue
    $m = [regex]::Match($text, 'DefaultExpectedSha256\s*=\s*''?([0-9a-fA-F]{64})''?')
    if (-not $m.Success) { Fail "no DefaultExpectedSha256 anchor found in $($f.Name)"; continue }
    $anchors[$f.Name] = $m.Groups[1].Value.ToLower()
    Write-Host "  anchor $($f.Name): $($anchors[$f.Name])"
}

# 期望值解析优先级与 deploy.ps1 对齐：param > sidecar > deploy.ps1 锚点
$expected = $null
if ($ExpectedSha256) { $expected = $ExpectedSha256.ToLower() }
else {
    $sidecar = "$SysPath.sha256"
    if (Test-Path -LiteralPath $sidecar) {
        $first = (Get-Content -LiteralPath $sidecar -TotalCount 1 -ErrorAction SilentlyContinue)
        if ($first -match '^([0-9a-fA-F]{64})') { $expected = $Matches[1].ToLower() }
    }
    if (-not $expected -and $anchors.ContainsKey('deploy.ps1')) { $expected = $anchors['deploy.ps1'] }
}
if (-not $expected) { Fail 'no expected SHA256 available (pass -ExpectedSha256 or add <sys>.sha256)' }

if ($expected -and $got -ne $expected) {
    Fail "sys hash does not match expected anchor (got=$got expected=$expected). Rebuild + sync the three anchors (deploy.ps1 / verify_deploy_signature.ps1 / README) or refuse to deploy."
}

# 三处锚点互为一致（红线：改哈希必须三处同步）
$distinct = @($anchors.Values | Sort-Object -Unique)
if ($distinct.Count -gt 1) {
    Fail "anchor files disagree with each other: $($anchors.Keys -join ', '). Sync all three (red line)."
}

if ($fail -eq 0) {
    Write-Host 'result: PASS (0 fail, 0 warn)'
    exit 0
} else {
    Write-Host 'result: FAIL'
    exit 1
}
