# RB-sign-deploy-147.ps1
# One elevated session: sign the freshly built driver -> stage into dist\ ->
# rewrite sidecar -> sync all three hash anchors -> verify chain -> DEPLOY to
# System32 + armed baseline -> post-verify (hash, signature, registry, DeployAck).
#
# Why one shot: R8 (one UAC per deployment). Signing needs the machine store
# (signtool /sm), deploy needs admin. Two sessions = two prompts.
#
# Deliberately does NOT reboot. Reboot is a separate, explicit decision (R8).
#
# ASCII-only + BOM by design: Gate18 check_ps_syntax rejects non-ASCII .ps1
# without BOM, and the checkout path contains CJK, so the repo root is derived
# from $PSScriptRoot and never written literally.

param(
    [string]$ExpectSha = 'eeab1dba7d21198faae0003441ff09d926f8f0f65c1417c1d2dea3cc77db3bab'
)

$ErrorActionPreference = 'Stop'

$LogPath    = Join-Path $env:TEMP 'rb-sign-deploy-148.log'
$ResultPath = Join-Path $env:TEMP 'rb-sign-deploy-148.result'
try { Start-Transcript -LiteralPath $LogPath -Force | Out-Null } catch {
    Write-Host "WARN: transcript unavailable ($($_.Exception.Message))"
}
Write-Host "log: $LogPath"
Write-Host "pid: $PID"

$Root    = Split-Path -Parent $PSScriptRoot
$Built   = Join-Path $Root 'src\driver\x64\Release\RAMBlockCache.sys'
$DistDir = Join-Path $Root 'dist'
$Dist    = Join-Path $DistDir 'RAMBlockCache.sys'
$Sidecar = Join-Path $DistDir 'RAMBlockCache.sys.sha256'
$Sync    = Join-Path $Root 'src\scripts\sync_anchors.ps1'
$Check   = Join-Path $Root 'src\scripts\check_signature.ps1'
$SignPs1 = Join-Path $Root 'src\setup\sign_driver.ps1'
$Deploy  = Join-Path $Root 'src\scripts\deploy.ps1'
$Sys32   = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
$SvcRoot = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$ClsKey  = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'

function Step([string]$m) { Write-Host ''; Write-Host "=== $m ===" -ForegroundColor Cyan }
function Ok([string]$m)   { Write-Host "  [ok] $m" -ForegroundColor Green }
function Die([string]$m)  {
    Write-Host "  [X] $m" -ForegroundColor Red
    try {
        [IO.File]::WriteAllText($ResultPath, ("FAIL: " + $m), (New-Object System.Text.UTF8Encoding($false)))
    } catch { }
    exit 1
}

# ---------------------------------------------------------------- 0. preflight
Step '0. preflight'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Die 'must run elevated (signtool /sm + System32 copy)'
}
Ok "elevated as $($id.Name)"

if (-not (Test-Path -LiteralPath $Built)) { Die "built sys missing: $Built" }
$sha  = (Get-FileHash -LiteralPath $Built -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item -LiteralPath $Built).Length
Write-Host "  artifact : $Built"
Write-Host "  sha256   : $sha"
Write-Host "  size     : $size"
if ($sha -ne $ExpectSha) {
    Die ("artifact drifted from the reviewed build`n   expected $ExpectSha`n   actual   $sha")
}
Ok 'artifact matches the reviewed build'

$excludeRe = '\\build\\|\\obj\\|\\bin\\|\\Release\\'
$newest = Get-ChildItem -Path (Join-Path $Root 'src\driver'), (Join-Path $Root 'src\core') `
    -Recurse -Include *.c, *.h -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch $excludeRe } |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $newest) { Die 'could not enumerate driver/core sources' }
if ($newest.LastWriteTimeUtc -gt (Get-Item -LiteralPath $Built).LastWriteTimeUtc) {
    Die "binary is OLDER than $($newest.Name) -- rebuild before signing"
}
Ok "binary newer than every driver/core source (newest: $($newest.Name))"

$bak = Join-Path $env:TEMP ("RAMBlockCache.unsigned." + $sha.Substring(0,8) + ".sys")
Copy-Item -LiteralPath $Built -Destination $bak -Force
Ok "unsigned copy saved: $bak"

# ------------------------------------------------------------------- 1. sign
Step '1. sign'
& powershell -NoProfile -ExecutionPolicy Bypass -File $SignPs1 `
    -Target $Built -ResultFile (Join-Path $env:TEMP 'rbsign_result.txt')
if ($LASTEXITCODE -ne 0) { Die "sign_driver.ps1 failed (exit $LASTEXITCODE)" }
Get-Content (Join-Path $env:TEMP 'rbsign_result.txt') | ForEach-Object { Write-Host "  $_" }

$sig = Get-AuthenticodeSignature -LiteralPath $Built
if ($sig.Status -ne 'Valid') { Die "post-sign status is $($sig.Status), expected Valid" }
Ok "signature Valid ($($sig.SignerCertificate.Subject))"
$signedSha = (Get-FileHash -LiteralPath $Built -Algorithm SHA256).Hash.ToLower()
Write-Host "  signed sha256 : $signedSha"

# ------------------------------------------------------- 2. stage into dist\
Step '2. stage into dist'
if (-not (Test-Path -LiteralPath $DistDir)) { New-Item -ItemType Directory -Path $DistDir | Out-Null }
$prev = if (Test-Path -LiteralPath $Dist) { (Get-FileHash -LiteralPath $Dist -Algorithm SHA256).Hash.ToLower() } else { '(none)' }
Write-Host "  previous dist sys : $prev"
Copy-Item -LiteralPath $Built -Destination $Dist -Force
$distSha = (Get-FileHash -LiteralPath $Dist -Algorithm SHA256).Hash.ToLower()
if ($distSha -ne $signedSha) { Die "dist copy hash mismatch" }
Ok "dist sys : $distSha"

[IO.File]::WriteAllText($Sidecar, "SHA256: $distSha`r`n", (New-Object System.Text.ASCIIEncoding))
$back = ((Get-Content -LiteralPath $Sidecar -TotalCount 1) -replace '^SHA256:\s*', '').Trim()
if ($back -ne $distSha) { Die "sidecar read-back mismatch: '$back'" }
Ok 'sidecar rewritten + read-back verified'

# -------------------------------------------------------- 3. sync anchors
Step '3. sync anchors (deploy.ps1 / verify_deploy_signature.ps1 / README.txt)'
& powershell -NoProfile -ExecutionPolicy Bypass -File $Sync
if ($LASTEXITCODE -ne 0) { Die "sync_anchors failed (exit $LASTEXITCODE)" }
Ok 'anchors synced'

Step '4. verify signature chain'
& powershell -NoProfile -ExecutionPolicy Bypass -File $Check
if ($LASTEXITCODE -ne 0) { Die "check_signature failed (exit $LASTEXITCODE)" }
Ok 'signature chain verified'

# ---------------------------------------------------------------- 5. deploy
Step '5. deploy (System32 copy + armed baseline + guards + tasks)'
& powershell -NoProfile -ExecutionPolicy Bypass -File $Deploy
$deployRc = $LASTEXITCODE
Write-Host "  deploy exit = $deployRc"
if ($deployRc -ne 0) { Die "deploy.ps1 failed (exit $deployRc); rollback already applied by deploy.ps1" }
Ok 'deploy.ps1 exit 0'

# ------------------------------------------------------------ 6. post-verify
Step '6. post-verify'
$liveSha  = (Get-FileHash -LiteralPath $Sys32 -Algorithm SHA256).Hash.ToLower()
$liveSig  = Get-AuthenticodeSignature -LiteralPath $Sys32
$liveMt   = (Get-Item -LiteralPath $Sys32).LastWriteTimeUtc.ToString('o')
$boot     = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
$p        = Get-ItemProperty -Path $SvcRoot
$uf       = (Get-ItemProperty -Path $ClsKey -Name UpperFilters).UpperFilters

Write-Host "  System32 sha256 : $liveSha"
Write-Host "  System32 sig    : $($liveSig.Status)"
Write-Host "  System32 mtimeZ : $liveMt"
Write-Host "  boot (Z)        : $($boot.ToString('o'))"

# The live sys is the image currently mapped by the kernel, so the file is held
# open; deploy.ps1 replaces it via MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT).
# In that case System32 legitimately still holds the PREVIOUS image and the hash
# MUST differ -- treating that as failure would misreport a good deploy.
$smKey   = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'
$pfro    = (Get-ItemProperty -Path $smKey -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
$pfroHit = @()
if ($pfro) { $pfroHit = @(@($pfro) | Where-Object { $_ -like '*RAMBlockCache.sys*' }) }
$pendingRb = ($pfroHit.Count -gt 0)
Write-Host "  pending replace : $pendingRb"
$pfroHit | ForEach-Object { Write-Host "    pfro: $_" }
Write-Host ("  DeploySeq={0} DeployAck={1} BootFailCount={2} LastBootHealthy={3} FilterArmed={4}" -f `
    $p.DeploySeq, $p.DeployAck, $p.BootFailCount, $p.LastBootHealthy, $p.FilterArmed)
Write-Host ("  UpperFilters    : [" + ($uf -join ', ') + "]")

if ($liveSha -ne $distSha) {
    if ($pendingRb) {
        Write-Host '  [..] System32 hash differs BY DESIGN: replacement is queued in PFRO and lands at next boot' -ForegroundColor Yellow
    } else {
        Die "System32 hash != dist hash and nothing is queued (live=$liveSha dist=$distSha)"
    }
} else {
    Ok 'System32 already holds the new image (direct copy, no reboot needed for the file)'
}
if ($liveSig.Status -ne 'Valid')      { Die "System32 signature is $($liveSig.Status)" }
# N-146-7 proof: the armed baseline must leave ack != seq so the next boot is graced.
if ([int]$p.DeploySeq -lt 1)          { Die 'DeploySeq was not armed' }
if ([int]$p.DeployAck -ne 0)          { Die "DeployAck=$($p.DeployAck) but must be 0 for the grace branch (N-146-7)" }
if ([int]$p.FilterArmed -ne 1)        { Die "FilterArmed=$($p.FilterArmed) but must be 1" }
if ([int]$p.BootFailCount -ne 0)      { Die "BootFailCount=$($p.BootFailCount) but must be 0" }
if ($uf -notcontains 'RAMBlockCache') { Die "UpperFilters missing RAMBlockCache: [$($uf -join ', ')]" }
Ok 'grace armed: DeploySeq > 0, DeployAck = 0, FilterArmed = 1, BootFailCount = 0'
if ($pendingRb) {
    Ok 'System32 image swap queued (PFRO) + signature Valid + UpperFilters armed'
} else {
    Ok 'System32 copy verified: hash + signature + UpperFilters'
}

# tasks must exist on disk (N-146-8 lesson: Register-* return value is not proof)
foreach ($t in @('RAMBoostBootGuard','RAMBoostLogonDisarm')) {
    $disk = Join-Path $env:SystemRoot "System32\Tasks\$t"
    if (-not (Test-Path -LiteralPath $disk)) { Die "scheduled task $t missing on disk after deploy" }
    Ok "task $t present on disk"
}

$summary = @"
OK
signed_sha256=$distSha
built_sha256=$sha
system32_sha256=$liveSha
pending_replace=$pendingRb
signature=$($liveSig.Status)
deploy_exit=$deployRc
DeploySeq=$($p.DeploySeq)
DeployAck=$($p.DeployAck)
BootFailCount=$($p.BootFailCount)
LastBootHealthy=$($p.LastBootHealthy)
FilterArmed=$($p.FilterArmed)
UpperFilters=$($uf -join ',')
boot_utc=$($boot.ToString('o'))
sys32_mtime_utc=$liveMt
log=$LogPath
"@
[IO.File]::WriteAllText($ResultPath, $summary, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ''
Write-Host '  DEPLOYED. No reboot performed.' -ForegroundColor Yellow
Write-Host '  Next: reboot once -> driver must NOT self-disable (grace armed) -> cache takes over.' -ForegroundColor Yellow
Write-Host "  result: $ResultPath"
try { Stop-Transcript | Out-Null } catch { }
exit 0
