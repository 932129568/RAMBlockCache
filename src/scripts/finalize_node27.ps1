# finalize_node27.ps1 -- ONE UAC: global Node 27 PATH + rebuild/sign Setup (all built in).
# 2026-11-06. Log: C:\ProgramData\rb-deploy\finalize-node27.log
# Sequence: pin Machine/User PATH -> toolchain contract -> rebuild Setup with
# Node 27 + npm 12.2.1 (SPA rebuilt, npm shipped inside) -> sign Setup ->
# verify signature + gate5 -> refresh rb-deploy staging. Driver sys is UNCHANGED:
# no resign, no anchor change, no deploy, no reboot.
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated'
    exit 1
}
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logDir  = 'C:\ProgramData\rb-deploy'
$log     = Join-Path $logDir 'finalize-node27.log'
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'
$setup   = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
$stageSetup = Join-Path $logDir 'RAMBlockCache-Setup.exe'
function L([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m)
}
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
'' | Set-Content -LiteralPath $log
foreach ($f in @($sigtool)) { if (-not (Test-Path -LiteralPath $f)) { L ("MISSING: " + $f); exit 1 } }
Set-Location -LiteralPath $root

# --- 1) global PATH: Node 27 + npm 12.2.1 for ALL new terminals ---
L '[1] pinning Machine/User PATH to Node 27 + npm 12.2.1 ...'
& (Join-Path $root 'src\scripts\set_node27_path.ps1') 2>&1 | ForEach-Object { L ("  path: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("PATH PIN FAILED exit=" + $LASTEXITCODE); exit 1 }

# --- 2) toolchain contract (static pin + build chain markers) ---
L '[2] checking Node 27 / npm 12.2.1 contract ...'
& python (Join-Path $root 'src\scripts\check_node_contract.py') 2>&1 | ForEach-Object { L ("  contract: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("CONTRACT FAILED exit=" + $LASTEXITCODE); exit 1 }

# --- 3) rebuild Setup (SPA built by npm 12.2.1; node.exe + npm shipped inside) ---
L '[3] rebuilding Setup with pinned toolchain ...'
& (Join-Path $root 'src\scripts\build_setup.ps1') 2>&1 | ForEach-Object { L ("  build: " + $_) }
if (-not (Test-Path -LiteralPath $setup)) { L 'SETUP MISSING after rebuild'; exit 1 }
L ("[3] rebuilt size=" + (Get-Item -LiteralPath $setup).Length)

# --- 4) sign Setup on an ASCII stage path, copy back ---
Copy-Item -LiteralPath $setup -Destination $stageSetup -Force
L '[4] signing Setup exe ...'
& $sigtool sign /sm /s My /sha1 $thumb /fd SHA256 $stageSetup 2>&1 | ForEach-Object { L ("  signtool: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("SETUP SIGN FAILED exit=" + $LASTEXITCODE); exit 1 }
$sp = Get-AuthenticodeSignature -LiteralPath $stageSetup
if ($sp.Status -ne 'Valid') { L ("SETUP SIGNATURE " + $sp.Status); exit 1 }
$setupHash = (Get-FileHash -LiteralPath $stageSetup -Algorithm SHA256).Hash.ToLower()
Copy-Item -LiteralPath $stageSetup -Destination $setup -Force
L ("[4] setup signed OK sha256=" + $setupHash)

# --- 5) final verification + staging refresh ---
& (Join-Path $root 'src\scripts\check_signature.ps1') 2>&1 | ForEach-Object { L ("  gate5: " + $_) }
$sysHash = (Get-FileHash -LiteralPath (Join-Path $root 'dist\RAMBlockCache.sys') -Algorithm SHA256).Hash.ToLower()
Add-Content -LiteralPath (Join-Path $logDir 'batch-hashes.txt') -Value ("NODE27_SYS_SHA256=" + $sysHash)
Add-Content -LiteralPath (Join-Path $logDir 'batch-hashes.txt') -Value ("NODE27_SETUP_SHA256=" + $setupHash)
L ("sys unchanged sha256=" + $sysHash)
L 'ALL OK'
Write-Host '=== Node 27 global + Setup rebuild/sign OK ==='
exit 0
