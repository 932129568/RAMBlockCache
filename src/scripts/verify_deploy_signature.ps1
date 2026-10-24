# verify_deploy_signature.ps1  [V10.4.3] Patch F: hard gate for the deployed sys
# Pure ASCII. Fail-closed verification of the driver BEFORE any reboot.
#
# Checks (all must pass, otherwise exit non-zero):
#   1. Source sys resolves (dist\RAMBlockCache.sys OR driver\x64\Release\RAMBlockCache.sys).
#   2. SHA256(source) == expected pin  (param > <sys>.sha256 sidecar > build pin).
#   3. signtool.exe present AND `signtool verify /pa` succeeds on the source sys.
#      A missing signtool is a FAILURE (no silent best-effort pass).
#   4. Deployed System32 copy matches the source hash. If it does not match, the
#      only accepted case is a queued replace-on-reboot (-AllowPendingReplace with a
#      PendingFileRenameOperations entry for the destination).
#
# Exit codes: 0 = verified, 2 = hash/deployed mismatch, 3 = signature/signtool failure,
#             1 = usage / missing input, 4 = I/O failure (source sys missing/unreadable,
#             Get-FileHash threw). [FIX S-03] 4 = I/O failure added; source-unreachable
#             used to fall into 1 (usage) with a raw exception stack.

[CmdletBinding()]
param(
    [string]$SourcePath        = "",
    [string]$ExpectedSha256    = "",
    [switch]$AllowPendingReplace
)

$ErrorActionPreference = 'Stop'

$DefaultExpectedSha256 = '5ebde9e28441d861b1df33508d3b2c7d3f72beeac7ef1d8f2cc324bac47a629d'

$scriptsDir = $PSScriptRoot
$root       = Split-Path -Parent $scriptsDir   # = src\
# [FIX 2026-10-17] 包根 = src\ 的上一级（dist\ 在包根，不在 src\ 下）——
# 与 deploy.ps1 同步修正，原 "$root\dist\..." 恒落空。
$repoRoot   = Split-Path -Parent $root
$dest       = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
$smKey      = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'

# [FIX S-04] PFRO 纯函数库（无顶层副作用）：dot-source 供下方 -AllowPendingReplace
# 成对匹配使用；QA 亦可独立 dot-source 本库做内存正/负例断言（不碰注册表）。
. (Join-Path $PSScriptRoot 'pfro_pending.ps1')

function Resolve-SysSource([string]$Explicit) {
    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
        throw "specified source sys not found: $Explicit"
    }
    $cands = @(
        (Join-Path $repoRoot 'dist\RAMBlockCache.sys'),            # 包根\dist = 签名部署产物（优先）
        (Join-Path $root 'driver\x64\Release\RAMBlockCache.sys')   # src\driver 编译产物（兜底，须经签名门禁）
    )
    foreach ($c in $cands) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    throw "source sys not found; looked in: $($cands -join ' ; ')"
}

function Resolve-ExpectedSha([string]$Sys, [string]$Explicit) {
    if ($Explicit) { return $Explicit.Trim().ToLower() }
    $sidecar = "$Sys.sha256"
    if (Test-Path -LiteralPath $sidecar) {
        $first = Get-Content -LiteralPath $sidecar -TotalCount 1
        # [FIX S-02] sidecar 首行哈希正则三处统一（deploy.ps1 / verify_deploy_signature.ps1 /
        # check_signature.ps1）：词边界匹配，禁行内任意位置取首条——"SHA256: <hash>" 格式
        # 必须三处同解；改此正则必须同步另两处。
        if ($first -match '(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])') { return $matches[0].ToLower() }
        throw "sidecar hash file has no 64-hex SHA256: $sidecar"
    }
    if ($DefaultExpectedSha256) { return $DefaultExpectedSha256.ToLower() }
    return $null
}

function Resolve-SignTool {
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $roots = @()
    if (${env:ProgramFiles(x86)}) { $roots += (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') }
    if ($env:ProgramFiles)        { $roots += (Join-Path $env:ProgramFiles        'Windows Kits\10\bin') }
    foreach ($r in $roots) {
        if ($r -and (Test-Path -LiteralPath $r)) {
            $hit = Get-ChildItem -LiteralPath $r -Recurse -Filter 'signtool.exe' -ErrorAction SilentlyContinue |
                   Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
                   Sort-Object FullName -Descending | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    return $null
}

try {
    $srcSys = Resolve-SysSource $SourcePath
} catch {
    # [FIX S-03] 源不可达归 I/O 失败（exit 4），不再混作 usage（exit 1）。
    Write-Host "[X] source sys not readable (I/O): $($_.Exception.Message)" -ForegroundColor Red
    exit 4
}
Write-Host "[..] source sys   : $srcSys"

if (-not (Test-Path -LiteralPath $dest)) {
    Write-Host "[X] deployed sys not found: $dest" -ForegroundColor Red
    exit 2
}

# --- expected hash resolution ---
try { $expected = Resolve-ExpectedSha $srcSys $ExpectedSha256 }
catch { Write-Host "[X] $($_.Exception.Message)" -ForegroundColor Red; exit 1 }
if (-not $expected) {
    Write-Host '[X] no expected SHA256 available (pass -ExpectedSha256 or add <sys>.sha256).' -ForegroundColor Red
    exit 1
}

# [FIX S-03] 两处 Get-FileHash 包 try/catch：文件不可读归 I/O 失败（exit 4），
# 不再让裸异常栈冒充 usage 错误。
try { $srcHash = (Get-FileHash -LiteralPath $srcSys -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower() }
catch {
    Write-Host "[X] cannot hash source sys (I/O): $($_.Exception.Message)" -ForegroundColor Red
    exit 4
}
try { $depHash = (Get-FileHash -LiteralPath $dest   -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower() }
catch {
    Write-Host "[X] cannot hash deployed sys (I/O): $($_.Exception.Message)" -ForegroundColor Red
    exit 4
}
Write-Host "source    : $srcHash"
Write-Host "deployed  : $depHash"
Write-Host "expected  : $expected"

# --- 1. source must match the expected pin ---
if ($srcHash -ne $expected) {
    Write-Host "[X] source SHA256 mismatch (source=$srcHash expected=$expected)." -ForegroundColor Red
    exit 2
}
Write-Host '[ok] source SHA256 matches expected pin.'

# --- 2. signature verification (hard requirement) ---
$signtool = Resolve-SignTool
if (-not $signtool) {
    Write-Host '[X] signtool.exe not found (PATH / Windows Kits). Signature cannot be verified.' -ForegroundColor Red
    exit 3
}
Write-Host "[..] signtool : $signtool"
& $signtool verify /pa /v $srcSys | Out-Host
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] signtool verify FAILED on source (exit $LASTEXITCODE)." -ForegroundColor Red
    exit 3
}
Write-Host '[ok] Authenticode signature verified.'

# --- 3. deployed copy must match the source ---
if ($depHash -eq $srcHash) {
    Write-Host '[ok] SHA256 match; deployed sys verified.'
    exit 0
}

# mismatch: accept only a queued replace-on-reboot
if ($AllowPendingReplace) {
    # [FIX S-04] 成对匹配：PFRO REG_MULTI_SZ 按 (源,目标) 步长 2 切对，源（归一化）==
    # 本次 $srcSys 且 目标 == $dest 才放行；陈旧队列（源是旧 dist 路径）/空队列 -> exit 2。
    # 旧实现：任意条目 -like '*RAMBlockCache.sys*' 即放行——陈旧部署队列可制造假 PASS。
    $pfro = (Get-ItemProperty -Path $smKey -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
    if (Test-PfroPendingReplace -PfroEntries @($pfro) -ExpectedSource $srcSys -ExpectedTarget $dest) {
        Write-Host '[ok] deployed copy differs but a matching (source,target) replace-on-reboot pair is queued; source verified.' -ForegroundColor Yellow
        exit 0
    }
}

Write-Host '[X] deployed SHA256 differs from source and no queued replace found; DO NOT REBOOT.' -ForegroundColor Red
exit 2
