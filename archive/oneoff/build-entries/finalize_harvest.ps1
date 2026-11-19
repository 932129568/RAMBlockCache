# finalize_harvest.ps1 -- ONE UAC at the end of the Verifier 1h harvest cycle.
# 2026-11-06. Log: C:\ProgramData\rb-deploy\finalize-harvest.log
# Sequence:
#   1) rebuild Setup (build_setup.ps1 -> SPA rebuilt by Node27/npm12.2.1,
#      webconsole.zip now carries the TinyLFU config-chain fix and the corrected
#      bridge/server/index.ts launcher path)
#   2) sign Setup.exe on an ASCII stage path, copy back, verify Valid
#   3) gate5 (signature chain + three anchors) -- sys is UNCHANGED, no sync/deploy
#   4) verifier /reset  (takes effect after the reboot)
# Driver config, registry and the running kernel image are untouched.
# A SINGLE REBOOT afterwards then proves: Verifier off, D: auto-resume,
# and the bridge coming back with the fixed modules.
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated'
    exit 1
}
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logDir  = 'C:\ProgramData\rb-deploy'
$log     = Join-Path $logDir 'finalize-harvest.log'
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'
$setup   = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
$stage   = Join-Path $logDir 'RAMBlockCache-Setup.exe'
function L([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m)
}
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
'' | Set-Content -LiteralPath $log
foreach ($f in @($sigtool, $setup)) { if (-not (Test-Path -LiteralPath $f)) { L ("MISSING: " + $f); exit 1 } }
Set-Location -LiteralPath $root

# --- 1) rebuild Setup with the fixed bridge sources ---
L '[1] rebuilding Setup (Node 27 + npm 12.2.1, payload refresh) ...'
& (Join-Path $root 'src\scripts\build_setup.ps1') 2>&1 | ForEach-Object { L ("  build: " + $_) }
if (-not (Test-Path -LiteralPath $setup)) { L 'SETUP MISSING after rebuild'; exit 1 }
L ("[1] rebuilt size=" + (Get-Item -LiteralPath $setup).Length)

# --- 2) sign Setup on an ASCII stage path ---
Copy-Item -LiteralPath $setup -Destination $stage -Force
L '[2] signing Setup exe ...'
& $sigtool sign /sm /s My /sha1 $thumb /fd SHA256 $stage 2>&1 | ForEach-Object { L ("  signtool: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("SETUP SIGN FAILED exit=" + $LASTEXITCODE); exit 1 }
$sp = Get-AuthenticodeSignature -LiteralPath $stage
if ($sp.Status -ne 'Valid') { L ("SETUP SIGNATURE " + $sp.Status); exit 1 }
$setupHash = (Get-FileHash -LiteralPath $stage -Algorithm SHA256).Hash.ToLower()
Copy-Item -LiteralPath $stage -Destination $setup -Force
L ("[2] setup signed OK sha256=" + $setupHash)

# --- 3) gate5: signature chain + three anchors (sys unchanged) ---
L '[3] gate5 ...'
& (Join-Path $root 'src\scripts\check_signature.ps1') 2>&1 | ForEach-Object { L ("  gate5: " + $_) }
if ($LASTEXITCODE -ne 0) { L 'GATE5 FAILED'; exit 1 }
$sysHash = (Get-FileHash -LiteralPath (Join-Path $root 'dist\RAMBlockCache.sys') -Algorithm SHA256).Hash.ToLower()
L ("[3] sys unchanged sha256=" + $sysHash)

# --- 4) reset Driver Verifier (effective after reboot) ---
L '[4] verifier /reset ...'
& verifier.exe /reset 2>&1 | ForEach-Object { L ("  verifier: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("VERIFIER RESET FAILED exit=" + $LASTEXITCODE); exit 1 }
L '[4] verifier reset OK (reboot required to take effect)'

Add-Content -LiteralPath (Join-Path $logDir 'batch-hashes.txt') -Value ("HARVEST_SETUP_SHA256=" + $setupHash)
Add-Content -LiteralPath (Join-Path $logDir 'batch-hashes.txt') -Value ("HARVEST_SYS_SHA256=" + $sysHash)
L 'ALL OK - reboot once to finish harvest (Verifier off + D: auto-resume)'
Write-Host '=== harvest finalize OK - reboot once ==='
exit 0
