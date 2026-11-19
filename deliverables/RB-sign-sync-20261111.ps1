# RB-sign-sync-20261111.ps1
# Sign the built driver, stage it into dist\, re-sync the three hash anchors,
# and verify the signature chain end to end.
#
# Scope: this script does NOT copy to System32 and does NOT reboot.
#        Deployment is a separate explicit step (see RB-fix-deploy notes).

$ErrorActionPreference = 'Stop'

# Transcript first: when this runs elevated the console closes on exit, so the
# log file is the only durable record. Must be started before any output.
$LogPath = Join-Path $env:TEMP 'rb-sign-sync-20261111.log'
try { Start-Transcript -LiteralPath $LogPath -Force | Out-Null } catch {
    Write-Host "WARN: transcript unavailable ($($_.Exception.Message)); continuing with console only"
}
Write-Host "log: $LogPath"
Write-Host "pid: $PID"

# ASCII-only file (Gate18 check_ps_syntax enforces BOM+ASCII for .ps1 that
# contain non-ASCII). Therefore NO literal repo path here: the checkout path
# contains CJK characters, so it is derived from $PSScriptRoot instead.
$Root    = Split-Path -Parent $PSScriptRoot   # deliverables\ -> repo root
$Built   = Join-Path $Root 'src\driver\x64\Release\RAMBlockCache.sys'
$DistDir = Join-Path $Root 'dist'
$Dist    = Join-Path $DistDir 'RAMBlockCache.sys'
$Sync    = Join-Path $Root 'src\scripts\sync_anchors.ps1'
$Check   = Join-Path $Root 'src\scripts\check_signature.ps1'
$SignPs1 = Join-Path $Root 'src\setup\sign_driver.ps1'

# The exact artifact this script was authored against. If the build has drifted
# we must stop rather than sign something we did not inspect.
$ExpectSha = '3c5b32a46f9bce58814ef9d8fb7cc01979589ec65355e09fdd878f764d730a0f'

function Step([string]$m) { Write-Host ""; Write-Host "=== $m ===" -ForegroundColor Cyan }
function Ok([string]$m)   { Write-Host "  [ok] $m" -ForegroundColor Green }
function Die([string]$m)  { Write-Host "  [X] $m" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- 0. preflight
Step '0. preflight'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Die "must run elevated (signtool /sm needs the machine store)"
}
Ok "elevated as $($id.Name)"

if (-not (Test-Path -LiteralPath $Built)) { Die "built sys missing: $Built" }
$sha = (Get-FileHash -LiteralPath $Built -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item -LiteralPath $Built).Length
Write-Host "  artifact : $Built"
Write-Host "  sha256   : $sha"
Write-Host "  size     : $size"
if ($sha -ne $ExpectSha) {
    Die ("artifact drifted from the reviewed build`n   expected $ExpectSha`n   actual   $sha`n   Re-review before signing.")
}
Ok 'artifact matches the reviewed build'

# source freshness: nothing under driver/core may be newer than the binary
$excludeRe = '\\build\\|\\obj\\|\\bin\\|\\Release\\'
$newest = Get-ChildItem -Path (Join-Path $Root 'src\driver'), (Join-Path $Root 'src\core') `
    -Recurse -Include *.c, *.h -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch $excludeRe } |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $newest) { Die 'could not enumerate driver/core sources -- refusing to sign blind' }
if ($newest.LastWriteTimeUtc -gt (Get-Item -LiteralPath $Built).LastWriteTimeUtc) {
    Die ("binary is OLDER than $($newest.Name) -- rebuild before signing")
}
Ok "binary newer than every driver/core source (newest: $($newest.Name))"

# keep a copy of the unsigned original for rollback/forensics
$bak = Join-Path $env:TEMP ("RAMBlockCache.unsigned." + $sha.Substring(0,8) + ".sys")
Copy-Item -LiteralPath $Built -Destination $bak -Force
Ok "unsigned copy saved: $bak"

# ------------------------------------------------------------------- 1. sign
Step '1. sign'
& powershell -NoProfile -ExecutionPolicy Bypass -File $SignPs1 `
    -Target $Built `
    -ResultFile (Join-Path $env:TEMP 'rbsign_result.txt')
if ($LASTEXITCODE -ne 0) { Die "sign_driver.ps1 failed (exit $LASTEXITCODE)" }
Get-Content (Join-Path $env:TEMP 'rbsign_result.txt') | ForEach-Object { Write-Host "  $_" }

$sig = Get-AuthenticodeSignature -LiteralPath $Built
if ($sig.Status -ne 'Valid') { Die "post-sign status is $($sig.Status), expected Valid" }
Ok "signature Valid ($($sig.SignerCertificate.Subject))"

$signedSha = (Get-FileHash -LiteralPath $Built -Algorithm SHA256).Hash.ToLower()
Write-Host "  signed sha256 : $signedSha"
if ($signedSha -eq $ExpectSha) {
    Write-Host "  note: hash unchanged by signing (already-hashed content)" -ForegroundColor Yellow
}

# ------------------------------------------------------- 2. stage into dist\
Step '2. stage into dist'
if (-not (Test-Path -LiteralPath $DistDir)) { New-Item -ItemType Directory -Path $DistDir | Out-Null }
$prev = if (Test-Path -LiteralPath $Dist) { (Get-FileHash -LiteralPath $Dist -Algorithm SHA256).Hash.ToLower() } else { '(none)' }
Write-Host "  previous dist sys : $prev"
Copy-Item -LiteralPath $Built -Destination $Dist -Force
$distSha = (Get-FileHash -LiteralPath $Dist -Algorithm SHA256).Hash.ToLower()
if ($distSha -ne $signedSha) { Die "dist copy hash mismatch: $distSha != $signedSha" }
Ok "dist sys : $distSha"

# the sidecar is normally hand-maintained; keep it consistent with dist.
# Format must stay "SHA256: <hash>" with CRLF -- three readers parse the first
# line (deploy.ps1 / verify_deploy_signature.ps1 / check_signature.ps1) and the
# established file is 74 bytes. A bare hash would still parse, but keeping the
# documented shape avoids a silent divergence from the other two anchors.
$sidecar = Join-Path $DistDir 'RAMBlockCache.sys.sha256'
[IO.File]::WriteAllText($sidecar, "SHA256: $distSha`r`n", (New-Object System.Text.ASCIIEncoding))
$back = ((Get-Content -LiteralPath $sidecar -TotalCount 1) -replace '^SHA256:\s*', '').Trim()
if ($back -ne $distSha) { Die "sidecar read-back mismatch: '$back' != '$distSha'" }
Ok "sidecar rewritten: $sidecar (read-back verified)"

# -------------------------------------------------------- 3. sync anchors
Step '3. sync anchors'
& powershell -NoProfile -ExecutionPolicy Bypass -File $Sync
if ($LASTEXITCODE -ne 0) { Die "sync_anchors failed (exit $LASTEXITCODE)" }
Ok 'anchors synced'

# ------------------------------------------------------------- 4. verify
Step '4. verify signature chain'
& powershell -NoProfile -ExecutionPolicy Bypass -File $Check
if ($LASTEXITCODE -ne 0) { Die "check_signature failed (exit $LASTEXITCODE)" }
Ok 'signature chain verified'

Step '5. summary'
Write-Host "  built   : $Built"
Write-Host "  dist    : $Dist"
Write-Host "  sha256  : $distSha"
Write-Host "  status  : Valid"
Write-Host ""
Write-Host '  NOT deployed. System32 untouched. No reboot performed.' -ForegroundColor Yellow
Write-Host '  Next step: deploy explicitly (single UAC), then one reboot.' -ForegroundColor Yellow
exit 0
