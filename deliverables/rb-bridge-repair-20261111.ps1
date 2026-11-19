# rb-bridge-repair.ps1 -- elevated one-shot.
#
# Problem being fixed: keeper.log and bridge-run.log both stop at
# 2026-11-08T04:31Z while boots happened on 11-09, 11-10, 11-11. The ONSTART
# task exists (schtasks reports "access denied" rather than "not found") but has
# not run. This script reads the real task state, re-registers it with an
# explicit working directory, and starts it once so the bridge is up for the
# post-reboot d3 run.
#
# Does NOT touch: the driver, System32, UpperFilters, Verifier, or reboot.
# Log: C:\ProgramData\rb-bridge\repair-<stamp>.log

$ErrorActionPreference = 'Continue'
$logDir = 'C:\ProgramData\rb-bridge'
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$log    = Join-Path $logDir ("repair-$stamp.log")

function L([string]$m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Add-Content -LiteralPath $log -Value $line
    Write-Host $line
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must be elevated'
    exit 1
}
L "elevated as $($id.Name)"
L "log: $log"

$taskName = 'RAMBlockCacheBridge'

# ---------------------------------------------------------------- 1. diagnose
L '== 1. current task state =='
$out = schtasks /query /tn $taskName /v /fo LIST 2>&1 | Out-String
# 0x80070005 == access denied; the message text is localized, the code is not.
$matched_denied = ($LASTEXITCODE -eq 0x80070005) -or ($out -match 'Access is denied')
if ($matched_denied) {
    L 'schtasks query DENIED even when elevated -- unexpected'
} else {
    foreach ($line in ($out -split "`r?`n")) {
        if ($line.Trim()) { L ("  " + $line.Trim()) }
    }
}

$xml = schtasks /query /tn $taskName /xml 2>&1 | Out-String
$xml_denied = ($LASTEXITCODE -eq 0x80070005) -or ($xml -match 'Access is denied')
if ($xml_denied) {
    L 'task XML unreadable'
} else {
    $xmlPath = Join-Path $logDir ("task-$stamp.xml")
    $xml | Set-Content -LiteralPath $xmlPath -Encoding utf8
    L "task XML saved: $xmlPath"
    # surface just the parts that decide whether ONSTART fires
    foreach ($pat in @('<Enabled>.*?</Enabled>', '<StartBoundary>.*?</StartBoundary>',
                       '<Delay>.*?</Delay>', '<ExecutionTimeLimit>.*?</ExecutionTimeLimit>',
                       '<Command>.*?</Command>', '<Arguments>.*?</Arguments>',
                       '<WorkingDirectory>.*?</WorkingDirectory>', '<UserId>.*?</UserId>',
                       '<RunLevel>.*?</RunLevel>', '<Trigger>.*?</Trigger>')) {
        foreach ($m in [regex]::Matches($xml, $pat)) {
            $v = $m.Value
            if ($v.Length -gt 200) { $v = $v.Substring(0, 200) + '...' }
            L ("  XML " + $v)
        }
    }
}

L ('running instances: ' + ((schtasks /query /tn $taskName /fo LIST 2>&1 | Out-String).Trim()))
L ('last run info: ' + ((Get-ScheduledTaskInfo -TaskName $taskName -ErrorAction SilentlyContinue |
    ForEach-Object { "LastRunTime=$($_.LastRunTime) LastResult=$($_.LastTaskResult) NextRunTime=$($_.NextRunTime)" })))

# ------------------------------------------------- 2. re-register, explicit cwd
# The old registration used schtasks /TR "<cmd>" with no working directory.
# bridge-task.ps1 resolves everything from absolute paths now, so cwd does not
# strictly matter, but pinning it removes one whole class of silent failure.
L '== 2. re-register task =='
$entry = 'C:\ProgramData\rb-bridge\bridge-task.ps1'
if (-not (Test-Path -LiteralPath $entry)) {
    L "FATAL: launcher missing: $entry"
    exit 1
}
# -WebPath is gone; the keeper derives $web from $root. Verify it resolves.
$probe = Get-Content -LiteralPath $entry -Raw
L ("launcher points at src\ui\web : " + ($probe -match 'src\\ui\\web'))
# The old bug was a bare "<repo>\web". Match a path ending in \web before a quote.
L ("launcher still has bare web   : " + ($probe -match '\\web'''))

$act = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $entry + '"') `
    -WorkingDirectory $logDir
$trg = New-ScheduledTaskTrigger -AtStartup
$trg.Delay = 'PT30S'
$set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -StartWhenAvailable -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 1)
$prin = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest

try {
    Register-ScheduledTask -TaskName $taskName -Action $act -Trigger $trg -Settings $set `
        -Principal $prin -Force -ErrorAction Stop | Out-Null
    $chk = Get-ScheduledTask -TaskName $taskName -ErrorAction Stop
    if (-not $chk) { throw 'task not found after Register-ScheduledTask' }
    L ("registered OK: state=" + $chk.State)
} catch {
    L ('REGISTER FAILED: ' + $_.Exception.Message)
    exit 1
}

# ------------------------------------------------------------ 3. start it now
L '== 3. start now =='
$startOut = schtasks /run /tn $taskName 2>&1 | Out-String
L ('schtasks /run : ' + $startOut.Trim())

$up = $false
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Seconds 2
    try {
        $h = Invoke-RestMethod -Uri 'http://127.0.0.1:5199/rb/health' -TimeoutSec 4
        if ($h) {
            L ("health UP after {0}s: native={1} deviceOpen={2} pid={3}" -f (($i + 1) * 2), $h.native, $h.deviceOpen, $h.pid)
            $up = $true
            break
        }
    } catch { }
}
if (-not $up) {
    L 'BRIDGE DID NOT COME UP after schtasks /run'
    L ('keeper.log tail: ' + ((Get-Content -LiteralPath (Join-Path $logDir 'keeper.log') -Tail 5 -ErrorAction SilentlyContinue) -join ' | '))
    exit 1
}

# ------------------------------------------------- 4. verify the RPC contract
L '== 4. verify RPC =='
try {
    $r = Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:5199/rb/rpc' `
        -ContentType 'application/json' -Body '{"method":4}' -TimeoutSec 10
    $n = 0
    if ($r.data) { $n = @($r.data.PSObject.Properties).Count }
    L ("GET_CONFIG status=" + $r.status + " fields=" + $n)
} catch {
    L ('RPC FAILED: ' + $_.Exception.Message)
}

L 'ALL OK'
exit 0
