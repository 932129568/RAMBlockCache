# phase4_close_race_probe.ps1 - READ-ONLY close-race evidence collector.
# Phase 4 / C-02. Collects the trigger + stability evidence required by
# KNOWN_ISSUES.md J17 and deliverables/175 (Verifier + Special Pool +
# directed interleave). It NEVER deploys, signs, reboots, writes registry,
# changes config, or enables write-back / WBCOAL (red lines R4/R9).
# See deliverables/design/phase4-close-race-verification-2026-11-13.md.
param(
    [int]$Minutes = 30,
    [int]$IntervalSec = 5,
    [switch]$InjectRefresh
)
$ErrorActionPreference = 'Continue'
$deploy = 'C:\ProgramData\rb-deploy'
$repo = $env:RB_REPO
$log = Join-Path $deploy ('phase4-close-race-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ') + '.log')
$script:lines = @()

function Say([string]$s) {
    $script:lines += $s
    Write-Host $s
}
function Flush-Lines {
    if ($script:lines.Count -gt 0) {
        Add-Content -LiteralPath $log -Value $script:lines -Encoding utf8
        $script:lines = @()
    }
}
function Get-VerifierInfo {
    $mm = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management' -ErrorAction SilentlyContinue
    if ($mm) {
        return ('VerifyDrivers=[' + ($mm.VerifyDrivers -join ',') + '] VerifyType=' + $mm.VerifyDriversType)
    }
    return 'VerifyDrivers=UNREADABLE'
}
function Get-SpecialPoolState {
    # verifier.exe output is localized, but the flag token is plain ASCII.
    try {
        $vq = (& verifier.exe /querysettings 2>&1 | Out-String)
        if ($vq -match '0x00000001') { return 'special_pool=ON' }
        return 'special_pool=UNCONFIRMED'
    } catch {
        return 'special_pool=UNREADABLE'
    }
}
function Get-SysInfo {
    $p = Join-Path $env:windir 'System32\drivers\RAMBlockCache.sys'
    if (-not (Test-Path -LiteralPath $p)) { return 'System32 sys MISSING' }
    $h = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower()
    $sg = (Get-AuthenticodeSignature -LiteralPath $p).Status
    return ('System32 sys sha=' + $h + ' sig=' + $sg)
}
function Get-CloseDeferredCount {
    $dir = Join-Path $env:ProgramData 'RAMBlockCache\logs'
    if (-not (Test-Path -LiteralPath $dir)) { return 0 }
    $n = 0
    foreach ($f in @(Get-ChildItem -LiteralPath $dir -Filter 'driver-*.log' -File -ErrorAction SilentlyContinue)) {
        $n += @(Select-String -LiteralPath $f.FullName -SimpleMatch 'l2: close deferred' -ErrorAction SilentlyContinue).Count
    }
    return $n
}
function Get-CrashCount {
    # No matching events is NOT a query failure: SilentlyContinue returns an
    # empty array, so a quiet 24h window is correctly reported as zero.
    $evts = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1001,41,6008; StartTime = (Get-Date).AddHours(-24) } -ErrorAction SilentlyContinue)
    return $evts.Count
}
function Get-DumpCount {
    $d = Join-Path $env:windir 'Minidump'
    if (-not (Test-Path -LiteralPath $d)) { return 0 }
    return @(Get-ChildItem -LiteralPath $d -Filter '*.dmp' -File -ErrorAction SilentlyContinue).Count
}

Say '== Phase 4 close-race evidence probe (READ ONLY) =='
Say ('start_utc=' + (Get-Date).ToUniversalTime().ToString('o'))
Say ('repo=' + $repo)
Say ('minutes=' + $Minutes + ' interval_sec=' + $IntervalSec + ' inject_refresh=' + [bool]$InjectRefresh)
Say (Get-VerifierInfo)
Say (Get-SpecialPoolState)
Say (Get-SysInfo)
$svc = Get-Service RAMBlockCache -ErrorAction SilentlyContinue
if ($svc) { Say ('service=' + $svc.Status + '/' + $svc.StartType) } else { Say 'service=MISSING' }
$pinFile = 'C:\Users\Public\o9\o9_fix_deploy_result.txt'
if (Test-Path -LiteralPath $pinFile) {
    $ms = [regex]::Matches((Get-Content -Raw -LiteralPath $pinFile), 'SIGNED sha=([0-9a-fA-F]{64})')
    if ($ms.Count -gt 0) { Say ('watch_pin=' + $ms[$ms.Count - 1].Groups[1].Value.ToLower()) }
}
if ($repo) {
    $anchor = Join-Path $repo 'dist\RAMBlockCache.sys.sha256'
    if (Test-Path -LiteralPath $anchor) { Say ('anchor=' + (Get-Content -Raw -LiteralPath $anchor).Trim()) }
}
$cd0 = Get-CloseDeferredCount
$cr0 = Get-CrashCount
$du0 = Get-DumpCount
Say ('baseline close_deferred=' + $cd0 + ' crash_events=' + $cr0 + ' dumps=' + $du0)
Say ('log=' + $log)
Flush-Lines

$deadline = (Get-Date).AddMinutes($Minutes)
$samples = 0
while ((Get-Date) -lt $deadline) {
    $samples++
    if ($InjectRefresh -and $repo) {
        $exe = Join-Path $repo 'src\ctl\rbfeatctl.exe'
        if (Test-Path -LiteralPath $exe) { & $exe refresh 2>&1 | Out-Null }
    }
    Start-Sleep -Seconds $IntervalSec
    if (($samples % 10) -eq 0) {
        $cd = Get-CloseDeferredCount
        Say ('sample=' + $samples + ' utc=' + (Get-Date).ToUniversalTime().ToString('o') + ' close_deferred=' + $cd)
        Flush-Lines
    }
}

$cd1 = Get-CloseDeferredCount
$cr1 = Get-CrashCount
$du1 = Get-DumpCount
$trigger = $cd1 - $cd0
$crashDelta = if ($cr0 -ge 0 -and $cr1 -ge 0) { $cr1 - $cr0 } else { -1 }
$dumpDelta = $du1 - $du0
$verifierOk = (Get-VerifierInfo) -match 'RAMBlockCache'
$specialPoolOk = ((Get-SpecialPoolState) -eq 'special_pool=ON')
Say ('final close_deferred=' + $cd1 + ' (delta=' + $trigger + ') crash_events=' + $cr1 + ' (delta=' + $crashDelta + ') dumps=' + $du1 + ' (delta=' + $dumpDelta + ')')
Say ('samples=' + $samples + ' verifier_target_ok=' + $verifierOk + ' special_pool_ok=' + $specialPoolOk)
if ($trigger -le 0) {
    Say 'VERDICT: INCONCLUSIVE - NO TRIGGER EVIDENCE (R7). The timeout branch was never exercised.'
} elseif ($crashDelta -lt 0) {
    Say 'VERDICT: INCONCLUSIVE - crash event query unavailable; stability cannot be asserted.'
} elseif ($crashDelta -gt 0 -or $dumpDelta -gt 0) {
    Say 'VERDICT: FAIL - new bugcheck/dump under the patched build. Reject the C-02 patch.'
} elseif (-not $verifierOk) {
    Say 'VERDICT: INCONCLUSIVE - Verifier is not targeting RAMBlockCache.sys.'
} elseif (-not $specialPoolOk) {
    Say 'VERDICT: INCONCLUSIVE - Special Pool is not confirmed active.'
} else {
    Say 'VERDICT: EVIDENCE COLLECTED - trigger + Verifier + no new crash. Send this log for independent review.'
}
Say ('end_utc=' + (Get-Date).ToUniversalTime().ToString('o'))
Flush-Lines
Write-Host ('log: ' + $log)
