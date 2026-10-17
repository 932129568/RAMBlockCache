# sync_anchors.ps1 - re-sync the deploy SHA256 anchors after rebuilding+signing the driver.
# Why: any source change -> rebuild -> new sys -> new hash. The three anchors below
# must match the DEPLOYED (signed) sys, or deploy.ps1 / verify_deploy_signature.ps1
# fail-closed (by design). This script computes the hash and rewrites all three.
# Usage (after signtool sign):
#   powershell -ExecutionPolicy Bypass -File src\scripts\sync_anchors.ps1 [-Sys <path>]
# Default -Sys: <repo>\dist\RAMBlockCache.sys
# Preserves each file's original BOM state (scripts/*.ps1 must keep BOM per
# check_ps_syntax gate; README.txt is whatever it was).
param(
    [string]$Sys = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Sys) { $Sys = Join-Path $root "dist\RAMBlockCache.sys" }
if (-not (Test-Path -LiteralPath $Sys)) { throw "sys not found: $Sys (sign first, then sync)" }

# fail-closed guard: refuse to anchor an unsigned binary
$sig = Get-AuthenticodeSignature -LiteralPath $Sys
if ($sig.Status -ne "Valid") {
    throw "sys signature status = $($sig.Status) - REFUSING to anchor an unsigned/invalid binary. Sign it first."
}

$hash = (Get-FileHash -LiteralPath $Sys -Algorithm SHA256).Hash.ToLower()
Write-Host "sys   : $Sys"
Write-Host "sha256: $hash"
Write-Host "status: $($sig.Status) ($($sig.SignerCertificate.Subject -replace '\s+',' '))"

$targets = @(
    (Join-Path $root "src\scripts\deploy.ps1"),
    (Join-Path $root "src\scripts\verify_deploy_signature.ps1"),
    (Join-Path $root "README.txt")
)
foreach ($f in $targets) {
    if (-not (Test-Path -LiteralPath $f)) { Write-Host "  [skip] missing: $f"; continue }
    $bytes = [IO.File]::ReadAllBytes($f)
    $hadBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $text = [IO.File]::ReadAllText($f)
    $rx = [regex]'(?m)^(?<pre>.*DefaultExpectedSha256\s*=\s*''?)(?<old>[0-9a-fA-F]{64})(?<post>''?.*)$'
    $new = $rx.Replace($text, { param($m) $m.Groups["pre"].Value + $hash + $m.Groups["post"].Value })
    if ($new -eq $text) { Write-Host "  [warn] no DefaultExpectedSha256 line changed in $f"; continue }
    $enc = New-Object System.Text.UTF8Encoding($hadBom)
    [IO.File]::WriteAllText($f, $new, $enc)
    Write-Host "  [ok] anchor updated: $f (bom=$hadBom)"
}
Write-Host "=== anchors synced. Now run: powershell -File src\scripts\deploy.ps1 (or deploy.cmd), then reboot ==="
