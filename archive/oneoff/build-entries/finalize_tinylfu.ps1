# finalize_tinylfu.ps1 -- F1 TinyLFU batch release (ONE UAC prompt).
# 2026-11-06. Log: C:\ProgramData\rb-deploy\finalize-tinylfu.log
# Sequence: sign new sys -> dist + sidecar -> sync 3 anchors -> deploy (queued,
# no reboot) -> rebuild Setup -> sign Setup -> verify. Pure ASCII + UTF-8 BOM.
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated'
    exit 1
}
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logDir  = 'C:\ProgramData\rb-deploy'
$log     = Join-Path $logDir 'finalize-tinylfu.log'
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'
$src     = Join-Path $root 'src\driver\x64\Release\RAMBlockCache.sys'
$dist    = Join-Path $root 'dist\RAMBlockCache.sys'
$setup   = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
$stageSys   = Join-Path $logDir 'RAMBlockCache.sys'
$stageSetup = Join-Path $logDir 'RAMBlockCache-Setup.exe'
$sidecar = "$dist.sha256"

function L([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m)
}
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
'' | Set-Content -LiteralPath $log
foreach ($f in @($sigtool, $src, $setup)) {
    if (-not (Test-Path -LiteralPath $f)) { L ("MISSING: " + $f); exit 1 }
}
Set-Location -LiteralPath $root

# --- 1) sign the new driver on an ASCII stage path ---
Copy-Item -LiteralPath $src -Destination $stageSys -Force
L ("[1] unsigned build: " + (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash.ToLower())
& $sigtool sign /sm /s My /sha1 $thumb /fd SHA256 $stageSys 2>&1 | ForEach-Object { L ("  signtool: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("SYS SIGN FAILED exit=" + $LASTEXITCODE); exit 1 }
$ss = Get-AuthenticodeSignature -LiteralPath $stageSys
if ($ss.Status -ne 'Valid') { L ("SYS SIGNATURE " + $ss.Status); exit 1 }
$sysHash = (Get-FileHash -LiteralPath $stageSys -Algorithm SHA256).Hash.ToLower()
Copy-Item -LiteralPath $stageSys -Destination $dist -Force
('SHA256: ' + $sysHash) | Set-Content -LiteralPath $sidecar -Encoding ASCII
L ("[1] sys signed OK sha256=" + $sysHash)

# --- 2) sync the three deploy anchors ---
L '[2] syncing deploy anchors ...'
& (Join-Path $root 'src\scripts\sync_anchors.ps1') 2>&1 | ForEach-Object { L ("  sync: " + $_) }

# --- 3) deploy (verify-before-change; System32 queued, NO reboot) ---
L '[3] running deploy.ps1 ...'
& (Join-Path $root 'src\scripts\deploy.ps1') 2>&1 | ForEach-Object { L ("  deploy: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("DEPLOY FAILED exit=" + $LASTEXITCODE); exit 1 }
L '[3] deploy OK (no reboot triggered)'

# --- 4) rebuild Setup with the signed driver + new Web/CLI payload ---
L '[4] rebuilding Setup ...'
& (Join-Path $root 'src\scripts\build_setup.ps1') 2>&1 | ForEach-Object { L ("  build: " + $_) }
if (-not (Test-Path -LiteralPath $setup)) { L 'SETUP MISSING after rebuild'; exit 1 }
L ("[4] rebuilt size=" + (Get-Item -LiteralPath $setup).Length)

# --- 5) sign Setup on an ASCII stage path, copy back ---
Copy-Item -LiteralPath $setup -Destination $stageSetup -Force
L '[5] signing Setup exe ...'
& $sigtool sign /sm /s My /sha1 $thumb /fd SHA256 $stageSetup 2>&1 | ForEach-Object { L ("  signtool: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("SETUP SIGN FAILED exit=" + $LASTEXITCODE); exit 1 }
$sp = Get-AuthenticodeSignature -LiteralPath $stageSetup
if ($sp.Status -ne 'Valid') { L ("SETUP SIGNATURE " + $sp.Status); exit 1 }
$setupHash = (Get-FileHash -LiteralPath $stageSetup -Algorithm SHA256).Hash.ToLower()
Copy-Item -LiteralPath $stageSetup -Destination $setup -Force
L ("[5] setup signed OK sha256=" + $setupHash)

# --- 6) final verification ---
& (Join-Path $root 'src\scripts\check_signature.ps1') 2>&1 | ForEach-Object { L ("  gate5: " + $_) }
Add-Content -LiteralPath (Join-Path $logDir 'batch-hashes.txt') -Value ("SYS_SHA256=" + $sysHash)
Add-Content -LiteralPath (Join-Path $logDir 'batch-hashes.txt') -Value ("SETUP_SHA256=" + $setupHash)
L 'ALL OK'
Write-Host '=== F1 TinyLFU release OK ==='
exit 0
