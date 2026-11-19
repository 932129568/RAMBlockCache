#requires -Version 5.1
<#
.SYNOPSIS
  Decisive, near-read-only probe: can the CONTROL path open the L2 container,
  and is there an index to load?

.DESCRIPTION
  Context (report 248, 2026-11-15):
  After the J6 prepare + ONE normal reboot, j6_reboot_hit.py reported
  L2state=0 with ZERO L2 counters while Features\Flags=0x4, and the driver log
  for that boot contains no 'l2:' line at all. The verifier's own 64KB sentinel
  read produced coreMiss=2, i.e. the miss DID reach RbFeatOnMiss, yet the
  container never opened.

  Code reading gives the reason: RbL2ProbeFill (src/driver/rbL2.c:1093) rejects
  when `((PRB_DEV_EXT)Extp)->VolId != s_VolId`, and s_VolId is only ever written
  inside the open path (rbL2.c:706). After a boot s_VolId == 0, so the FIRST
  open can never come from the data path -- only from a control path
  (REFRESH -> RbL2LazyOpen(TRUE)).

  This probe issues exactly one REFRESH (flags reload + idempotent open; it
  writes NO configuration) and compares stats before/after. It answers:
    * does the control path open the container?  (s_State 0 -> 1)
    * does L2 load an index?                     (.idx still 4 B -> nothing)
    * are probes now served from an empty index? (L2Miss grows, L2Hits stays 0)

  It is NOT a J6 verdict and it relaxes nothing. Write-back / WBCOAL stay OFF.
  No registry write, no service control, no reboot, no driver build/sign/deploy.

.OUTPUTS
  Exit 0 = probe completed (read the printed verdict).
  Exit 2 = probe could not run (missing files / not elevated).
#>
[CmdletBinding()]
param(
    [string]$Repo = ''
)

$ErrorActionPreference = 'Stop'

function Write-Row([string]$Tag, [string]$Text) {
    Write-Host ('  [' + $Tag.PadRight(6) + '] ' + $Text)
}

# Blank separator deliberately emitted as a single space: under output
# redirection an empty Write-Host (and even Write-Gap) is
# buffered on a different stream than Write-Host and the line disappears,
# which glued section headers onto the previous row.
function Write-Gap { Write-Host ' ' }

# --- locate repo + tooling ------------------------------------------------
if ([string]::IsNullOrWhiteSpace($Repo)) {
    $probe = Split-Path -Parent $PSCommandPath
    for ($i = 0; $i -lt 4 -and $probe; $i++) {
        if (Test-Path -LiteralPath (Join-Path $probe 'src\ctl\rbfeatctl.exe')) {
            $Repo = $probe
            break
        }
        $probe = Split-Path -Parent $probe
    }
}
$ctl = Join-Path $Repo 'src\ctl\rbfeatctl.exe'
if (-not (Test-Path -LiteralPath $ctl)) {
    Write-Host ('[X] rbfeatctl.exe not found under repo: ' + $Repo)
    exit 2
}

$idxPath = 'C:\RAMBoostL2c.idx'
$binPath = 'C:\RAMBoostL2c.bin'

Write-Host '=================================================================='
Write-Host '  RAMBlockCache L2 CONTROL-PATH OPEN PROBE (near read-only)'
Write-Host '  one REFRESH; no config write; write-back / WBCOAL stay OFF'
Write-Host '=================================================================='
Write-Host ('  repo : ' + $Repo)
Write-Host ('  utc  : ' + (Get-Date).ToUniversalTime().ToString('o'))

# --- helpers --------------------------------------------------------------
function Get-IdxState {
    if (-not (Test-Path -LiteralPath $idxPath)) { return 'ABSENT' }
    $i = Get-Item -LiteralPath $idxPath
    return ('{0} B  mtime={1}' -f $i.Length, $i.LastWriteTimeUtc.ToString('o'))
}

function Get-StatsSnapshot {
    $r = [ordered]@{
        ok = $false; err = ''; state = $null
        l2s = $null; l2h = $null; l2m = $null
        coreHit = $null; coreMiss = $null; flags = $null
    }
    try {
        $jsonOut = (& $ctl stats -json 2>&1 | ForEach-Object { [string]$_ }) -join "`n"
        $jsonRc = $LASTEXITCODE
    }
    catch {
        $jsonOut = [string]$_.Exception.Message
        $jsonRc = -1
    }
    if ($jsonRc -ne 0) {
        $r.err = 'stats -json exit=' + $jsonRc + ' (elevation needed?)'
        return $r
    }
    $m = [regex]::Match($jsonOut, '\{[^\r\n]*\}')
    if (-not $m.Success) {
        $r.err = 'no JSON object in stats -json output'
        return $r
    }
    try {
        $j = $m.Value | ConvertFrom-Json
    }
    catch {
        $r.err = 'stats JSON did not parse: ' + $_.Exception.Message
        return $r
    }
    $r.flags = $j.flags
    $r.coreHit = $j.hits
    $r.coreMiss = $j.miss
    $r.l2s = $j.l2s
    $r.l2h = $j.l2h
    $r.l2m = $j.l2m
    try {
        $textOut = (& $ctl stats 2>&1 | ForEach-Object { [string]$_ }) -join "`n"
        $mt = [regex]::Match($textOut, 'L2state=(\d+)')
        if ($mt.Success) { $r.state = [int]$mt.Groups[1].Value }
    }
    catch {
        $r.state = $null
    }
    $r.ok = $true
    return $r
}

function Format-Stats($s) {
    if (-not $s.ok) { return 'UNREADABLE (' + $s.err + ')' }
    return ('flags=0x{0:X} state={1} coreHit={2} coreMiss={3} L2Stores={4} L2Hits={5} L2Miss={6}' -f `
            [int]$s.flags, $s.state, $s.coreHit, $s.coreMiss, $s.l2s, $s.l2h, $s.l2m)
}

# --- baseline -------------------------------------------------------------
Write-Gap
Write-Host '-- 0) baseline --'
$idxBefore = Get-IdxState
$binSize = if (Test-Path -LiteralPath $binPath) { (Get-Item -LiteralPath $binPath).Length } else { -1 }
Write-Row 'info' ('index     : ' + $idxBefore)
Write-Row 'info' ('container : ' + $binSize + ' B')

$before = Get-StatsSnapshot
Write-Row 'info' ('pre       : ' + (Format-Stats $before))
if (-not $before.ok) {
    Write-Gap
    Write-Host '[X] cannot read stats; run this probe ELEVATED (the .cmd wrapper does it).'
    exit 2
}

# --- the single control-path action --------------------------------------
Write-Gap
Write-Host '-- 1) control path: refresh (flags reload + idempotent L2 open) --'
$null = & $ctl refresh 2>&1
$rrc = $LASTEXITCODE
Write-Row 'info' ('refresh exit = ' + $rrc)
Start-Sleep -Seconds 2

$after = Get-StatsSnapshot
Write-Row 'info' ('post      : ' + (Format-Stats $after))
$idxAfter = Get-IdxState
Write-Row 'info' ('index now : ' + $idxAfter)

# --- verdict --------------------------------------------------------------
Write-Gap
Write-Host '-- 2) reading (each line is an observation, not a J6 verdict) --'

$opened = ($before.state -ne 1) -and ($after.ok -and $after.state -eq 1)
$stillClosed = ($after.ok -and $after.state -ne 1)

if ($opened) {
    Write-Row 'OBS' 'control path OPENED the container (state went to 1)'
    Write-Row 'OBS' '=> data path alone could NOT open it: rbL2.c:1093 VolId guard vs s_VolId==0'
} elseif ($stillClosed) {
    Write-Row 'OBS' ('control path did NOT open it either (state=' + $after.state + ')')
    Write-Row 'OBS' '=> look for an l2: refusal line in the driver log; this is a different gate'
}

$idxGrew = ($idxBefore -ne 'ABSENT') -and ($idxAfter -ne $idxBefore)
if ($idxGrew) {
    Write-Row 'OBS' 'index header changed size/mtime after refresh'
} else {
    Write-Row 'OBS' 'index unchanged (still short/unsealed) => nothing to load: cold start'
}

if ($after.ok -and $before.ok -and $null -ne $after.l2m) {
    $dL2m = [int]$after.l2m - [int]$before.l2m
    $dL2h = [int]$after.l2h - [int]$before.l2h
    $dL2s = [int]$after.l2s - [int]$before.l2s
    Write-Row 'OBS' ('delta over refresh: dL2Stores=' + $dL2s + ' dL2Hits=' + $dL2h + ' dL2Miss=' + $dL2m)
}

Write-Gap
Write-Host '-- 3) driver log, last l2 lines this boot --'
try {
    $log = Join-Path $env:ProgramData ('RAMBlockCache\logs\driver-' + (Get-Date).ToString('yyyyMMdd') + '.log')
    if (Test-Path -LiteralPath $log) {
        $lines = @(Get-Content -LiteralPath $log -Tail 40 | Where-Object { $_ -match 'l2:|unseal|idx' })
        if ($lines.Count -eq 0) {
            Write-Row 'info' 'no l2:/unseal/idx line in the last 40 log lines'
        } else {
            foreach ($ln in $lines) { Write-Row 'info' $ln }
        }
    } else {
        Write-Row 'info' ('driver log not present: ' + $log)
    }
}
catch {
    Write-Row 'info' ('driver log read failed: ' + $_.Exception.Message)
}

Write-Gap
Write-Host 'Reminder: this probe changed NO configuration. To restore the default'
Write-Host 'posture run deliverables\RB-l2-p0c-rollback.cmd (clears Features\Flags L2).'
exit 0
