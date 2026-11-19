# File: src/scripts/bridge_watchdog.ps1
# Purpose: Supervise the local Node 27 HTTP bridge on 127.0.0.1:5199 and call
#          the existing restart_bridge.ps1 after two consecutive failures.
# Encoding: UTF-8 with BOM, CRLF, pure ASCII only.
# Origin: deliverables/240-full-stack-architecture-guidance-2026-11-14.md 4A.
# NOTE: this script does NOT register itself as a scheduled task. The sandbox
# cannot manage SYSTEM tasks, and the observation window is still active.
# Run it with -RunOnce for a read-only probe, or under an existing supervisor.

param(
    [string]$HealthUrl = 'http://127.0.0.1:5199/health',
    [int]$PollIntervalSec = 10,
    [int]$TimeoutSec = 3,
    [switch]$RunOnce
)

$ErrorActionPreference = 'Continue'
$restartScript = Join-Path $PSScriptRoot 'restart_bridge.ps1'

function Test-BridgeAlive {
    param([string]$Url, [int]$Timeout)
    try {
        $resp = Invoke-WebRequest -Uri $Url -Method Get -TimeoutSec $Timeout -ErrorAction Stop
        return ($resp.StatusCode -ge 200 -and $resp.StatusCode -lt 500)
    } catch {
        try {
            $tcp = New-Object System.Net.Sockets.TcpClient
            $async = $tcp.BeginConnect('127.0.0.1', 5199, $null, $null)
            $connected = $async.AsyncWaitHandle.WaitOne(($Timeout * 1000), $false)
            if ($connected -and $tcp.Connected) {
                $tcp.EndConnect($async)
                $tcp.Close()
                return $true
            }
            $tcp.Close()
            return $false
        } catch {
            return $false
        }
    }
}

$consecutiveFails = 0
while ($true) {
    $alive = Test-BridgeAlive -Url $HealthUrl -Timeout $TimeoutSec
    if ($alive) {
        if ($consecutiveFails -gt 0) {
            Write-Host ('[WATCHDOG] Bridge recovered after {0} failure(s).' -f $consecutiveFails)
        }
        $consecutiveFails = 0
    } else {
        $consecutiveFails++
        Write-Host ('[WATCHDOG] Bridge unreachable (attempt {0}).' -f $consecutiveFails)
        if ($consecutiveFails -ge 2) {
            if (Test-Path -LiteralPath $restartScript) {
                Write-Host '[WATCHDOG] Invoking restart_bridge.ps1...'
                & $restartScript
            } else {
                Write-Host ('[WATCHDOG] restart_bridge.ps1 not found at {0}' -f $restartScript)
            }
        }
    }
    if ($RunOnce) { break }
    Start-Sleep -Seconds $PollIntervalSec
}
