# File: src/scripts/l1_perf_orchestrator.ps1
# Purpose: Audit R2 live driver parity and apply ObserveBaseline (Flags=0) or L1Fast (Flags=3).
# Encoding: UTF-8 with BOM, CRLF, Pure ASCII ONLY.
# Origin: deliverables/243-ios26-liquid-glass-and-fullstack-wiring-2026-11-15.md 4.4
# [243 2026-11-15] Supersedes the 240-era -ApplyL1Fast switch. Dual mode keeps
# the observation window (09:30 watch expects Flags=0) and the post-window
# performance profile (PREFETCH|HOT) from being confused with each other.
# The CLI contract is the real one: ramboostctl.exe set takes key=value pairs
# (block / tinylfu / parallel), and the ctl binaries live in src/ctl, not dist.

param(
    [ValidateSet('AuditOnly', 'ObserveBaseline', 'L1Fast')]
    [string]$Mode = 'AuditOnly',
    [ValidateSet(4096, 8192, 16384, 32768, 65536)]
    [int]$BlockSize = 4096,
    [ValidateSet(1, 2, 4, 8, 16)]
    [int]$ParallelFlush = 8
)

$ErrorActionPreference = 'Continue'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$distSys  = Join-Path $repoRoot 'dist\RAMBlockCache.sys'
$liveSys  = Join-Path $env:windir 'System32\drivers\RAMBlockCache.sys'
$ctlDir   = Join-Path $repoRoot 'src\ctl'
$ramCtl   = Join-Path $ctlDir 'ramboostctl.exe'
$featCtl  = Join-Path $ctlDir 'rbfeatctl.exe'
$regFeat  = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features'

Write-Host '[1/4] Checking R2 live driver hash parity...'
if (-not (Test-Path $distSys)) {
    throw "Anchored dist driver not found: $distSys"
}
$distHash = (Get-FileHash -Path $distSys -Algorithm SHA256).Hash.ToLowerInvariant()
$liveHash = if (Test-Path $liveSys) { (Get-FileHash -Path $liveSys -Algorithm SHA256).Hash.ToLowerInvariant() } else { 'MISSING' }

Write-Host ("      dist SHA256 : {0}" -f $distHash)
Write-Host ("      live SHA256 : {0}" -f $liveHash)

if ($distHash -ne $liveHash) {
    Write-Warning 'R2 EXCEPTION WINDOW IS OPEN: System32 live binary does not match anchored dist binary!'
    Write-Warning 'Run deliverables\RB-r2-restore-step1.cmd -> reboot -> step2.cmd first.'
    if ($Mode -ne 'AuditOnly') {
        throw "Refusing -Mode $Mode while R2 exception window is open."
    }
}

Write-Host '[2/4] Running native sigverify check...'
if (Test-Path $ramCtl) {
    & $ramCtl sigverify $distSys
    if (Test-Path $liveSys) {
        & $ramCtl sigverify $liveSys
    }
}

Write-Host '[3/4] Reading Features\Flags via Get-ItemProperty...'
if (Test-Path $regFeat) {
    $props = Get-ItemProperty -Path $regFeat -ErrorAction SilentlyContinue
    if ($null -ne $props -and $null -ne $props.Flags) {
        $curFlags = [uint32]$props.Flags
        Write-Host ("      Current Flags = {0} (0x{0:X})" -f $curFlags)
    } else {
        Write-Host '      Flags value absent under Features key.'
    }
} else {
    Write-Host '      Features key absent (driver defaults to Flags=0).'
}

Write-Host '[4/4] Querying live volume list and stats...'
if (Test-Path $featCtl) {
    & $featCtl vols
    & $featCtl stats -json
}

if ($Mode -eq 'ObserveBaseline') {
    Write-Host '[APPLY] Restoring observation-window safe baseline (Flags=0, BlockSize=4096, TinyLFU=0)...'
    if (Test-Path $ramCtl) {
        # [FIX 2026-11-15] ramboostctl 'block' is expressed in KB, NOT bytes.
        # Passing block=4096 means 4096 KB, which is outside the legal list
        # {4/8/16/32/64 KB} and silently falls back to 4096 B with a warning.
        # 4 KB is what we want here, so send block=4.
        & $ramCtl set 'block=4' 'tinylfu=0' 'parallel=1'
        if ($LASTEXITCODE -ne 0) {
            throw "ramboostctl set failed with exit code $LASTEXITCODE"
        }
    }
    if (Test-Path $regFeat) {
        Set-ItemProperty -Path $regFeat -Name 'Flags' -Value 0 -Type DWord -ErrorAction Stop
    }
    Write-Host '[NEXT] Baseline restored. Keep Flags=0 until observation window ends (2026-11-16).'
}
elseif ($Mode -eq 'L1Fast') {
    Write-Host ("[APPLY] Applying L1Fast profile (PREFETCH|HOT=3, BlockSize={0}, TinyLFU=1, ParallelFlush={1})..." -f $BlockSize, $ParallelFlush)
    if (Test-Path $ramCtl) {
        # [FIX 2026-11-15] 'block' takes KB. Sending raw bytes made every
        # non-4K profile fall back to 4096 B (e.g. block=32768 = 32 MB was
        # rejected and clamped), so -BlockSize had no effect at all.
        $blockKb = [int]($BlockSize / 1024)
        & $ramCtl set ("block={0}" -f $blockKb) 'tinylfu=1' ("parallel={0}" -f $ParallelFlush)
        if ($LASTEXITCODE -ne 0) {
            throw "ramboostctl set failed with exit code $LASTEXITCODE"
        }
    }
    if (Test-Path $regFeat) {
        # Bitmask 3 = RB2F_PREFETCH (0x1) | RB2F_HOT (0x2); L2 (0x4) and WBCOAL (0x8) cleared
        Set-ItemProperty -Path $regFeat -Name 'Flags' -Value 3 -Type DWord -ErrorAction Stop
    }
    Write-Host '[NEXT] Profile written. Attach a non-system data volume in Volumes UI or run rbfeatctl.exe vols.'
}
