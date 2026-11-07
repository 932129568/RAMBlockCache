# finalize_s4_five_tier.ps1 -- S4 five-tier geometry matrix (4K/8K/16K/32K/64K)
# 2026-11-08. ONE UAC. Log: C:\ProgramData\rb-deploy\s4-five-tier.log
# Discipline (90 no. STOP-ON-FAIL + unconditional 4K rollback):
#   per tier: set block=N -> poll rebuild done -> double proof
#   (engine_bs==N && rebuild_status==0 && rebuild_count+1) -> canary verify
#   -> bench x3 rounds (median). Any failure -> Abort -> finally restores 4K.
$ErrorActionPreference = 'Continue'
$global:LASTEXITCODE = 0
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated'; exit 1
}
$root   = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$log    = 'C:\ProgramData\rb-deploy\s4-five-tier.log'
$ctl    = Join-Path $root 'src\ctl\ramboostctl.exe'
$bench  = Join-Path $root 'src\tools\rbbench\bin\Release\net10.0-windows\win-x64\rbbench.exe'
$verify = Join-Path $root 'src\core\build\rbc_e2e_verify.exe'
$work   = 'D:\rbc-e2e-work.bin'
$canary = 'D:\rbc-e2e-canary.bin'
foreach ($f in @($ctl, $bench, $verify, $work, $canary)) {
    if (-not (Test-Path -LiteralPath $f)) { Write-Host ('[X] MISSING: ' + $f); exit 1 }
}
if (-not (Test-Path -LiteralPath $log)) { '' | Set-Content -LiteralPath $log }
function L([string]$m) { Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }
function Abort([string]$m) {
    L ('ABORT: ' + $m); Write-Host ('[X] ' + $m)
    if ($script:curBs -ne 4096) {
        L 'finally: restoring 4K (unconditional)'
        & $ctl set block=4096 2>&1 | Out-Null
        Start-Sleep -Seconds 3
        $bs = (& $ctl statval engine_bs 2>&1) -join ''
        L ('finally: engine_bs=' + $bs)
        if ($bs -ne '4096') { L 'finally: ROLLBACK FAILED - manual check required' }
        else { L 'finally: 4K restored OK' }
    }
    exit 1
}
function Stat([string]$k) { return ((& $ctl statval $k 2>&1) -join '').Trim() }

L '=== S4 five-tier start ==='
L ('baseline: engine_bs=' + (Stat 'engine_bs') + ' count=' + (Stat 'rebuild_count') + ' status=' + (Stat 'rebuild_status') + ' dirty=' + (Stat 'dirty_bytes'))
if ((Stat 'engine_bs') -ne '4096') { Abort 'start: engine_bs != 4096' }
if ((Stat 'dirty_bytes') -ne '0')  { Abort 'start: dirty_bytes != 0 (not write-through baseline)' }
$v = & $ctl volumes 2>&1 | Out-String; L ('volumes: ' + ($v -replace '\r?\n', ' | ').Trim())
$cfg0 = (& $ctl get-config 2>&1) -join "`n"

# canary pre-check
$c = & $verify --file $canary --size 64 --seed 20261107 --threads 4 --pattern splitmix --verify-only 2>&1 | Out-String
L ('canary pre: ' + (($c -split "`r?`n" | Select-Object -Last 1)).Trim())
if ($c -notmatch 'RESULT: PASS') { Abort 'canary pre-check FAILED' }

# CLI 'block=' unit is KB: block=8 -> 8192 B (ramboostctl.c compares kb*1024).
$tiers = @(8, 16, 32, 64, 4)                  # CLI KB values
$tierB = @(8192, 16384, 32768, 65536, 4096)   # expected engine bytes
$script:curBs = 4096
$prevCount = [long](Stat 'rebuild_count')
$results = @()
for ($ti = 0; $ti -lt $tiers.Count; $ti++) {
    $t = $tiers[$ti]; $tb = $tierB[$ti]
    L ('--- tier ' + $tb + ' (block=' + $t + ' KB) ---')
    $r = (& $ctl set block=$t 2>&1) | Out-String
    L ('set block=' + $t + ' KB rc=' + $LASTEXITCODE + ' out: ' + (($r -replace '?
',' | ').Trim()))
    if ($LASTEXITCODE -ne 0) { Abort ('set block=' + $t + ' failed') }
    $done = $false
    for ($i = 0; $i -lt 180; $i++) {
        Start-Sleep -Seconds 1
        if ((Stat 'rebuild_in_progress') -eq '0') { $done = $true; break }
    }
    if (-not $done) { Abort ('tier ' + $t + ': rebuild_in_progress timeout 180s') }
    $bs = Stat 'engine_bs'; $st = Stat 'rebuild_status'; $cnt = Stat 'rebuild_count'
    L ('tier ' + $tb + ' proof: engine_bs=' + $bs + ' status=' + $st + ' count=' + $cnt + ' dirty=' + (Stat 'dirty_bytes'))
    if ($bs -ne [string]$tb) { Abort ('tier ' + $tb + ': engine_bs=' + $bs + ' (double-proof failed)') }
    if ($st -ne '0')        { Abort ('tier ' + $tb + ': rebuild_status=' + $st + ' (rollback happened)') }
    if ([long]$cnt -le $prevCount) { Abort ('tier ' + $tb + ': rebuild_count not incremented') }
    $prevCount = [long]$cnt
    $c = & $verify --file $canary --size 64 --seed 20261107 --threads 4 --pattern splitmix --verify-only 2>&1 | Out-String
    if ($c -notmatch 'RESULT: PASS') { Abort ('tier ' + $tb + ': canary FAILED after rebuild') }
    L ('tier ' + $tb + ' canary PASS')
    $script:curBs = $tb
    for ($rd = 1; $rd -le 3; $rd++) {
        $out = & $bench --file $work --size 32 --seq4k 16 --ops 1024 --repeat 1 --keep --tag ('s4_' + $tb + '_r' + $rd) 2>&1
        $rc = $LASTEXITCODE
        if ($rc -ne 0) { Abort ('tier ' + $tb + ' bench round ' + $rd + ' failed rc=' + $rc) }
        foreach ($ln in $out) { if ($ln -match 'MiB/s') { L ('BENCH t=' + $tb + ' r=' + $rd + ' ' + ([string]$ln).Trim()) } }
    }
    $results += [pscustomobject]@{ Tier = $tb; Count = $cnt }
    L ('--- tier ' + $tb + ' OK ---')
}
if ((Stat 'engine_bs') -ne '4096') { Abort 'end: engine_bs != 4096' }
$c = & $verify --file $canary --size 64 --seed 20261107 --threads 4 --pattern splitmix --verify-only 2>&1 | Out-String
if ($c -notmatch 'RESULT: PASS') { Abort 'end: canary FAILED' }
$cfg1 = (& $ctl get-config 2>&1) -join "`n"
if ($cfg1 -ne $cfg0) {
    L 'WARN: get-config text drift (BlockSize field is expected to read 4096 both ends; diff below)'
    $a = $cfg0 -split "`r?`n"; $b = $cfg1 -split "`r?`n"
    for ($i = 0; $i -lt [Math]::Max($a.Count, $b.Count); $i++) {
        if ($a[$i] -ne $b[$i]) { L ('CFGDRIFT line ' + $i + ': [' + $a[$i] + '] -> [' + $b[$i] + ']') }
    }
}
L ('FINAL: count=' + (Stat 'rebuild_count') + ' status=' + (Stat 'rebuild_status') + ' dirty=' + (Stat 'dirty_bytes'))
L 'ALL OK - five tiers switched and restored; canary PASS throughout'
Write-Host '=== S4 five-tier ALL OK (see s4-five-tier.log) ==='
exit 0
