# File: src/scripts/check_l1_accel_readiness.ps1
# Purpose: READ-ONLY L1 acceleration readiness inspection. No writes, no reboot.
# Encoding: UTF-8 with BOM, CRLF, pure ASCII only.
# Origin: deliverables/239-full-stack-liquid-glass-guidance-2026-11-14.md sec 4.2.
# The sample embedded a CJK repository path, which violates the pure-ASCII
# rule. The repo root is derived from $PSScriptRoot instead.

$ErrorActionPreference = 'Continue'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$distDir  = Join-Path $repoRoot 'dist'
# Real ctl output directory comes from build/sources.manifest.json targets.ctl.outDir.
# dist/ holds ONLY the signed .sys, Setup.exe and the .sha256 anchor.
$ctlDir   = Join-Path $repoRoot 'src\ctl'
$distSys  = Join-Path $distDir 'RAMBlockCache.sys'
$featCtl  = Join-Path $ctlDir 'rbfeatctl.exe'
$liveSys  = Join-Path $env:windir 'System32\drivers\RAMBlockCache.sys'
$regFeat  = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features'

Write-Host '[1/4] Checking Features Flags registry value...'
if (Test-Path -LiteralPath $regFeat) {
    $props = Get-ItemProperty -Path $regFeat -ErrorAction Stop
    if ($null -eq $props) { throw 'Get-ItemProperty returned null; refusing a vacuous PASS.' }
    Write-Host ('      Current Flags = {0} (safe default = 0, L1-fast suggestion = 3)' -f $props.Flags)
    if (($props.Flags -band 0x4) -ne 0) { Write-Host '      NOTE: RB2F_L2 (0x4) is SET; Phase 4 keeps L2 frozen.' }
    if (($props.Flags -band 0x8) -ne 0) { Write-Host '      WARNING: RB2F_WBCOAL (0x8) is SET (R4 red line).' }
} else {
    Write-Host '      Features key not present (driver defaults to Flags=0).'
}

Write-Host '[2/4] Comparing anchored dist image with live System32 image...'
if (Test-Path -LiteralPath $distSys) {
    $distHash = (Get-FileHash -LiteralPath $distSys -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host ('      dist SHA256 = {0}' -f $distHash)
} else {
    Write-Host '      dist driver MISSING.'
}
if (Test-Path -LiteralPath $liveSys) {
    $liveHash = (Get-FileHash -LiteralPath $liveSys -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host ('      live SHA256 = {0}' -f $liveHash)
    if ($distHash -and ($distHash -ne $liveHash)) {
        Write-Host '      R2 EXCEPTION WINDOW ACTIVE: live image differs from anchored dist.'
        Write-Host '      NEXT: run the R2 restore package, reboot once, then VerifyRestore.'
    }
} else {
    Write-Host '      live System32 driver MISSING.'
}

Write-Host '[3/4] Querying live volumes via rbfeatctl.exe vols...'
if (Test-Path -LiteralPath $featCtl) {
    & $featCtl vols 2>&1 | Out-Host
    Write-Host ('      vols exit code = {0} (5 means "not elevated")' -f $LASTEXITCODE)
} else {
    Write-Host '      rbfeatctl.exe not found in dist.'
}

Write-Host '[4/4] Querying live counters via rbfeatctl.exe stats -json...'
if (Test-Path -LiteralPath $featCtl) {
    & $featCtl stats -json 2>&1 | Out-Host
    Write-Host ('      stats exit code = {0} (5 means "not elevated")' -f $LASTEXITCODE)
}

Write-Host 'READ-ONLY COMPLETE: no registry write, no config change, no reboot.'
Write-Host 'Any live configuration change requires an explicit user decision.'
