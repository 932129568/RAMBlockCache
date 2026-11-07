# restart_bridge.ps1 -- ONE UAC: reload the user-mode bridge so the TinyLFU
# config-chain fix (proto-decode.ts decodeConfig / index.ts encodeParams) is
# loaded into the live process. Verifies GET_CONFIG returns 24 fields incl TinyLFU.
# 2026-11-06. Log: C:\ProgramData\rb-deploy\restart-bridge.log
# Does NOT touch driver config, registry values, Verifier, or reboot the machine.
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated (bridge node runs as SYSTEM)'
    exit 1
}
$logDir = 'C:\ProgramData\rb-deploy'
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$log = Join-Path $logDir 'restart-bridge.log'
function L([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m)
}
'' | Set-Content -LiteralPath $log

$healthUri = 'http://127.0.0.1:5199/rb/health'
$rpcUri    = 'http://127.0.0.1:5199/rb/rpc'

# ---- 1) current bridge pid (H-5: a DOWN bridge is RECOVERABLE, not fatal) ----
$healthOk = $true
try { $h = Invoke-RestMethod -Uri $healthUri -TimeoutSec 6 }
catch {
    $healthOk = $false
    L ("HEALTH FAILED (bridge DOWN): " + $_.Exception.Message)
}

if (-not $healthOk) {
    # This script is the elevated wrapper; the SYSTEM/ONSTART task is the
    # supported way to bring the bridge back when BOTH node and keeper died.
    L 'bridge DOWN -> schtasks /Run /TN RAMBlockCacheBridge, then bounded re-check'
    $runOut = & schtasks /Run /TN RAMBlockCacheBridge
    L ("schtasks /Run exit=" + $LASTEXITCODE + " :: " + ($runOut -join ' '))
    $h = $null
    for ($i = 0; $i -lt 15; $i++) {
        Start-Sleep -Seconds 2
        try {
            $hr = Invoke-RestMethod -Uri $healthUri -TimeoutSec 4
            if ($hr) { $h = $hr; break }
        } catch { }
    }
    if ($null -eq $h) {
        L 'RECOVERY FAILED: bridge still DOWN after schtasks /Run'
        Write-Host '[X] bridge DOWN and the scheduled task did not recover it.'
        Write-Host '    Check: schtasks /Query /TN RAMBlockCacheBridge /V'
        Write-Host '           keeper in C:\ProgramData\rb-bridge, web\dist\index.html present.'
        Write-Host '    Then re-run this script (no reboot / no driver change needed).'
        exit 1
    }
    $newPid = [int]$h.pid
    L ("recovered from DOWN: pid=" + $newPid + " (fresh process; skip kill/relaunch)")
} else {
    $oldPid = [int]$h.pid
    L ("before: pid={0} node={1} native={2}" -f $oldPid, $h.node, $h.native)

    # ---- 2) stop it (keeper in C:\ProgramData\rb-bridge checks 5199 every 5s
    #         and relaunches node from the repo web/ directory) ----
    try { Stop-Process -Id $oldPid -Force -ErrorAction Stop; L ("stopped pid=" + $oldPid) }
    catch { L ("STOP FAILED: " + $_.Exception.Message); exit 1 }

    # ---- 3) wait for the new process (bounded check, not a retry loop) ----
    $newPid = 0
    for ($i = 0; $i -lt 25; $i++) {
        Start-Sleep -Seconds 2
        try {
            $h2 = Invoke-RestMethod -Uri $healthUri -TimeoutSec 4
            if ([int]$h2.pid -ne $oldPid) { $newPid = [int]$h2.pid; break }
        } catch { }
    }
    if ($newPid -eq 0) { L 'BRIDGE DID NOT COME BACK (keeper may be stopped)'; exit 1 }
    L ("after: pid=" + $newPid)
}

# ---- 4) verify the live GET_CONFIG decoder ----
Start-Sleep -Seconds 2
try {
    $r = Invoke-RestMethod -Method Post -Uri $rpcUri -ContentType 'application/json' -Body '{"method":4}' -TimeoutSec 8
    $d = $r.data
    $n = @($d.PSObject.Properties).Count
    $tl = $d.TinyLFU
    L ("GET_CONFIG: fields={0} TinyLFU={1} status={2}" -f $n, $tl, $r.status)
    if ($n -ne 24 -or $null -eq $tl) { L 'VERIFY FAILED: expected 24 fields with TinyLFU'; exit 1 }
} catch { L ("RPC FAILED: " + $_.Exception.Message); exit 1 }

L 'ALL OK (bridge reloaded; config chain fixed; driver config untouched)'
Write-Host '=== bridge restart OK ==='
exit 0
