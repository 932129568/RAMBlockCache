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
#             1 = usage / missing input.

[CmdletBinding()]
param(
    [string]$SourcePath        = "",
    [string]$ExpectedSha256    = "",
    [switch]$AllowPendingReplace
)

$ErrorActionPreference = 'Stop'

$DefaultExpectedSha256 = 'b6fb097ba24fab1621df48ca066cebd57d1a9cc55015c5737d69e27e3dd64882'

$scriptsDir = $PSScriptRoot
$root       = Split-Path -Parent $scriptsDir   # = src\
# [FIX 2026-10-17] 包根 = src\ 的上一级（dist\ 在包根，不在 src\ 下）——
# 与 deploy.ps1 同步修正，原 "$root\dist\..." 恒落空。
$repoRoot   = Split-Path -Parent $root
$dest       = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
$smKey      = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'

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
        if ($first -match '([0-9a-fA-F]{64})') { return $matches[1].ToLower() }
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
    Write-Host "[X] $($_.Exception.Message)" -ForegroundColor Red
    exit 1
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

$srcHash = (Get-FileHash -LiteralPath $srcSys -Algorithm SHA256).Hash.ToLower()
$depHash = (Get-FileHash -LiteralPath $dest   -Algorithm SHA256).Hash.ToLower()
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
    $pfro = (Get-ItemProperty -Path $smKey -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
    $queued = $false
    if ($pfro) {
        foreach ($entry in $pfro) {
            if ($entry -like '*RAMBlockCache.sys*') { $queued = $true; break }
        }
    }
    if ($queued) {
        Write-Host '[ok] deployed copy differs but a replace-on-reboot is queued; source verified.' -ForegroundColor Yellow
        exit 0
    }
}

Write-Host '[X] deployed SHA256 differs from source and no queued replace found; DO NOT REBOOT.' -ForegroundColor Red
exit 2
