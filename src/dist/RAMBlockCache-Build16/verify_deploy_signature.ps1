# =====================================================================
# [ARCHIVE - Build 16] HISTORICAL COPY - DO NOT USE.
# This snapshot is kept only as a ROLLBACK REFERENCE: read-only, do not
# delete (refactor spec E.6 / P1-5).
#
# Key differences vs the current src/scripts/verify_deploy_signature.ps1:
#   * Only compares the local sys hash with the already-deployed System32
#     copy; there is no expected-hash pin and no Authenticode check.
#   * Exits 0 when no deployed sys exists in System32 ("nothing deployed"
#     treated as OK) - the current version is fail-closed (missing
#     signtool = FAIL, no expected hash = FAIL, mismatch = DO NOT REBOOT).
# Use scripts/verify_deploy_signature.ps1 instead.
#
# This header is the ONLY change made to this snapshot file.
# =====================================================================
$ErrorActionPreference = 'Stop'
$PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

# Find local sys: try flat package layout first, then project tree layout
$candidates = @(
    (Join-Path $PSScriptRoot 'driver\x64\Release\RAMBlockCache.sys'),
    (Join-Path $PSScriptRoot 'driver\RAMBlockCache.sys'),
    (Join-Path $PSScriptRoot '..\driver\x64\Release\RAMBlockCache.sys'),
    (Join-Path $PSScriptRoot '..\driver\RAMBlockCache.sys')
)
$localSys = $null
foreach ($c in $candidates) {
    if (Test-Path $c) { $localSys = $c; break }
}
if (-not $localSys) {
    Write-Error "local sys not found in any of: $($candidates -join '; ')"
    exit 2
}
Write-Host "using local sys: $localSys"

$sysDest = "$env:SystemRoot\System32\drivers\RAMBlockCache.sys"
if (-not (Test-Path $sysDest)) {
    Write-Warning "system32 sys not found (will be copied on next deploy)"
    exit 0
}
$h1 = (Get-FileHash $localSys -Algorithm SHA256).Hash
$h2 = (Get-FileHash $sysDest -Algorithm SHA256).Hash
if ($h1 -ne $h2) {
    Write-Error "HASH MISMATCH local=$h1 system=$h2"
    exit 2
}
Write-Host "OK: local and system sys SHA256 match: $h1"