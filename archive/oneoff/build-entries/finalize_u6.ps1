# finalize_u6.ps1 -- U-6: sign silent engine, repack Setup, re-sign Setup.
# 2026-11-06. One UAC prompt. Log: C:\ProgramData\rb-deploy\finalize-u6.log
# Pure ASCII + UTF-8 BOM (check_ps_syntax gate 4). No source edits.
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated'
    exit 1
}
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logDir  = 'C:\ProgramData\rb-deploy'
$log     = Join-Path $logDir 'finalize-u6.log'
$sigtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$thumb   = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'
$engine  = Join-Path $root 'src\setup\ramboostsetup.exe'
$setup   = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
$sys     = Join-Path $root 'dist\RAMBlockCache.sys'
$engineStage = Join-Path $logDir 'ramboostsetup.exe'
$setupStage  = Join-Path $logDir 'RAMBlockCache-Setup.exe'

function L([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m)
}

if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
'' | Set-Content -LiteralPath $log
foreach ($f in @($sigtool, $engine, $setup, $sys)) {
    if (-not (Test-Path -LiteralPath $f)) { L ("MISSING: " + $f); exit 1 }
}

# --- 1) sign the silent engine on an ASCII stage path, copy back to repo ---
Copy-Item -LiteralPath $engine -Destination $engineStage -Force
L '[1] signing silent engine ...'
& $sigtool sign /sm /s My /sha1 $thumb /fd SHA256 $engineStage 2>&1 | ForEach-Object { L ("  signtool: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("ENGINE SIGN FAILED exit=" + $LASTEXITCODE); exit 1 }
$es = Get-AuthenticodeSignature -LiteralPath $engineStage
if ($es.Status -ne 'Valid') { L ("ENGINE SIGNATURE " + $es.Status); exit 1 }
Copy-Item -LiteralPath $engineStage -Destination $engine -Force
L ("[1] engine signed OK sha256=" + (Get-FileHash -LiteralPath $engine -Algorithm SHA256).Hash.ToLower())

# --- 2) repack Setup so the payload contains the SIGNED engine ---
L '[2] rebuilding Setup with signed engine ...'
Set-Location -LiteralPath $root
& (Join-Path $root 'src\scripts\build_setup.ps1') 2>&1 | ForEach-Object { L ("  build: " + $_) }
if (-not (Test-Path -LiteralPath $setup)) { L 'SETUP MISSING after rebuild'; exit 1 }
L ("[2] rebuilt size=" + (Get-Item -LiteralPath $setup).Length)

# --- 3) sign the freshly built Setup on an ASCII stage path, copy back ---
Copy-Item -LiteralPath $setup -Destination $setupStage -Force
L '[3] signing Setup exe ...'
& $sigtool sign /sm /s My /sha1 $thumb /fd SHA256 $setupStage 2>&1 | ForEach-Object { L ("  signtool: " + $_) }
if ($LASTEXITCODE -ne 0) { L ("SETUP SIGN FAILED exit=" + $LASTEXITCODE); exit 1 }
$ss = Get-AuthenticodeSignature -LiteralPath $setupStage
if ($ss.Status -ne 'Valid') { L ("SETUP SIGNATURE " + $ss.Status); exit 1 }
$hash = (Get-FileHash -LiteralPath $setupStage -Algorithm SHA256).Hash.ToLower()
Copy-Item -LiteralPath $setupStage -Destination $setup -Force
L ("[3] setup signed OK sha256=" + $hash)

# --- 4) confirm the sys anchor is untouched (no re-deploy needed) ---
$sysSig = Get-AuthenticodeSignature -LiteralPath $sys
L ("[4] sys sha256=" + (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash.ToLower() + " sig=" + $sysSig.Status)
L 'ALL OK'
Write-Host '=== U-6 round OK ==='
exit 0
