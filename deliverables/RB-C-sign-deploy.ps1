# RB-C-sign-deploy.ps1
# C-plan (J18): sign the freshly built hashed-slot driver -> stage into dist\ ->
# rewrite sidecar -> sync all three hash anchors -> verify chain -> DEPLOY to
# System32 + armed baseline -> post-verify.
#
# One elevated session on purpose (R8): signtool /sm needs the machine store and
# deploy needs admin, so two sessions would mean two UAC prompts.
# Deliberately does NOT reboot. Reboot is a separate, explicit decision.
#
# ASCII-only + BOM by design: check_ps_syntax rejects non-ASCII .ps1 without BOM,
# and the checkout path contains CJK, so the repo root is derived from
# $PSScriptRoot and never written literally.
#
# See deliverables/206-l2-c-plan-deploy-runbook-2026-11-12.md

param(
    [string]$ExpectSha = '8d70f541739d577b5b0cb4a3e611d601c16ab7cfd402f39caad4e910ce1f51b2'
)

$ErrorActionPreference = 'Stop'

$LogPath    = Join-Path $env:TEMP 'rb-C-sign-deploy.log'
$ResultPath = Join-Path $env:TEMP 'rb-C-sign-deploy.result'
try { Start-Transcript -LiteralPath $LogPath -Force | Out-Null } catch {
    Write-Host "WARN: transcript unavailable ($($_.Exception.Message))"
}
Write-Host "log: $LogPath"
Write-Host "pid: $PID"

$Root     = Split-Path -Parent $PSScriptRoot
$Built    = Join-Path $Root 'src\driver\x64\Release\RAMBlockCache.sys'
$DistDir  = Join-Path $Root 'dist'
$Dist     = Join-Path $DistDir 'RAMBlockCache.sys'
$Sidecar  = Join-Path $DistDir 'RAMBlockCache.sys.sha256'
$Sync     = Join-Path $Root 'src\scripts\sync_anchors.ps1'
$Check    = Join-Path $Root 'src\scripts\check_signature.ps1'
$SignPs1  = Join-Path $Root 'src\setup\sign_driver.ps1'
$Deploy   = Join-Path $Root 'src\scripts\deploy.ps1'
$L2Gate   = Join-Path $Root 'src\scripts\check_l2_contract.py'
$Sys32    = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
$SvcRoot  = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
$ClsKey   = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'

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
    Die ("artifact drifted from the reviewed C-plan build`n   expected $ExpectSha`n   actual   $sha")
}
Ok 'artifact matches the reviewed C-plan build'

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

# [C-PLAN] the C-plan fix lives in kernel code that core 66 does NOT compile.
# The ONLY automated proof for it is the static contract gate; refuse to sign
# if it drifted. Use 'Continue' + explicit $LASTEXITCODE so native stderr does
# not abort the elevated pipeline (18:04 UAC lesson).
if (-not (Test-Path -LiteralPath $L2Gate)) { Die "missing L2 contract gate: $L2Gate" }
$ErrorActionPreference = 'Continue'
& python $L2Gate 2>&1 | ForEach-Object { Write-Host "  $_" }
$l2rc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($l2rc -ne 0) { Die "L2 static contract gate failed (exit $l2rc) -- refusing to sign" }
Ok 'L2 static contract gate PASS'

$bak = Join-Path $env:TEMP ("RAMBlockCache.C-unsigned." + $sha.Substring(0,8) + ".sys")
Copy-Item -LiteralPath $Built -Destination $bak -Force
Ok "unsigned copy saved: $bak"

# ------------------------------------------------------------------- 1. sign
Step '1. sign'
$ErrorActionPreference = 'Continue'
& powershell -NoProfile -ExecutionPolicy Bypass -File $SignPs1 `
    -Target $Built -ResultFile (Join-Path $env:TEMP 'rb-C-sign-result.txt')
$signRc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($signRc -ne 0) { Die "sign_driver.ps1 failed (exit $signRc)" }
if (Test-Path (Join-Path $env:TEMP 'rb-C-sign-result.txt')) {
    Get-Content (Join-Path $env:TEMP 'rb-C-sign-result.txt') | ForEach-Object { Write-Host "  $_" }
}

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
Step '3. sync anchors'
# sync_anchors.ps1 is fail-closed itself: it refuses to anchor a non-Valid binary.
$ErrorActionPreference = 'Continue'
& powershell -NoProfile -ExecutionPolicy Bypass -File $Sync
$syncRc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($syncRc -ne 0) { Die "sync_anchors failed (exit $syncRc)" }
Ok 'anchors synced'

Step '4. verify signature chain'
$ErrorActionPreference = 'Continue'
& powershell -NoProfile -ExecutionPolicy Bypass -File $Check
$checkRc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($checkRc -ne 0) { Die "check_signature failed (exit $checkRc)" }
Ok 'signature chain verified'

# ---------------------------------------------------------------- 5. deploy
Step '5. deploy (System32 copy + armed baseline + guards + tasks)'
$ErrorActionPreference = 'Continue'
& powershell -NoProfile -ExecutionPolicy Bypass -File $Deploy
$deployRc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
Write-Host "  deploy exit = $deployRc"
if ($deployRc -ne 0) { Die "deploy.ps1 failed (exit $deployRc); rollback already applied by deploy.ps1" }
Ok 'deploy.ps1 exit 0'

# ------------------------------------------------------------ 6. post-verify
Step '6. post-verify'
$liveSha = (Get-FileHash -LiteralPath $Sys32 -Algorithm SHA256).Hash.ToLower()
$liveSig = Get-AuthenticodeSignature -LiteralPath $Sys32
$liveMt  = (Get-Item -LiteralPath $Sys32).LastWriteTimeUtc.ToString('o')
$boot    = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
$p       = Get-ItemProperty -Path $SvcRoot
$uf      = (Get-ItemProperty -Path $ClsKey -Name UpperFilters).UpperFilters

Write-Host "  System32 sha256 : $liveSha"
Write-Host "  System32 sig    : $($liveSig.Status)"
Write-Host "  System32 mtimeZ : $liveMt"
Write-Host "  boot (Z)        : $($boot.ToString('o'))"

# The live sys is held open by the kernel; deploy.ps1 replaces it via
# MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT). In that case System32 legitimately
# still holds the PREVIOUS image until reboot.
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
        Write-Host '  [..] System32 hash differs BY DESIGN: replacement queued in PFRO, lands at next boot' -ForegroundColor Yellow
    } else {
        Die "System32 hash != dist hash and nothing is queued (live=$liveSha dist=$distSha)"
    }
} else {
    Ok 'System32 already holds the new image'
}
if ($liveSig.Status -ne 'Valid')      { Die "System32 signature is $($liveSig.Status)" }
if ([int]$p.DeploySeq -lt 1)          { Die 'DeploySeq was not armed' }
if ([int]$p.DeployAck -ne 0)          { Die "DeployAck=$($p.DeployAck) but must be 0 for the grace branch" }
if ([int]$p.FilterArmed -ne 1)        { Die "FilterArmed=$($p.FilterArmed) but must be 1" }
if ([int]$p.BootFailCount -ne 0)      { Die "BootFailCount=$($p.BootFailCount) but must be 0" }
if ($uf -notcontains 'RAMBlockCache') { Die "UpperFilters missing RAMBlockCache: [$($uf -join ', ')]" }
Ok 'grace armed: DeploySeq > 0, DeployAck = 0, FilterArmed = 1, BootFailCount = 0'

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
Write-Host '  DEPLOYED (C-plan). No reboot performed.' -ForegroundColor Yellow
Write-Host '  Next: reboot once -> tell the agent -> P5 acceptance with -PostC.' -ForegroundColor Yellow
Write-Host "  result: $ResultPath"
try { Stop-Transcript | Out-Null } catch { }
exit 0
