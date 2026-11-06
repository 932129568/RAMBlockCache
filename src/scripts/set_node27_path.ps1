# set_node27_path.ps1 -- pin Node 27 + npm 12.2.1 GLOBALLY (Machine + User PATH).
# 2026-11-06, user decision "system path too, all built in".
# Requires elevation (Machine scope). Run via RB-finalize-node27.cmd.
# Log: C:\ProgramData\rb-deploy\node27-path.log
param([string]$Node27 = 'D:\node-v27.0.0-alpha.0.0.0-win-x64')
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated (Machine PATH)'
    exit 1
}
$logDir = 'C:\ProgramData\rb-deploy'
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$log = Join-Path $logDir 'node27-path.log'
function L([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m)
}
'' | Set-Content -LiteralPath $log

$nodeExe = Join-Path $Node27 'node.exe'
$npmCli  = Join-Path $Node27 'node_modules\npm\bin\npm-cli.js'
if (-not (Test-Path -LiteralPath $nodeExe) -or -not (Test-Path -LiteralPath $npmCli)) {
    L ("MISSING node27 toolchain at " + $Node27); exit 1
}
$nv = (& $nodeExe --version 2>$null)
$np = (& $nodeExe $npmCli --version --loglevel=error 2>$null | Select-Object -Last 1)
if ("$nv" -notlike 'v27.*' -or "$np" -notlike '12.2.1*') {
    L ("BAD TOOLCHAIN node=$nv npm=$np"); exit 1
}
L ("toolchain OK: node=$nv npm=$np dir=$Node27")

$target = $Node27.TrimEnd('\')
foreach ($scope in @('Machine', 'User')) {
    $cur = [Environment]::GetEnvironmentVariable('Path', $scope)
    if (-not $cur) { $cur = '' }
    $parts = @()
    foreach ($p in ($cur -split ';')) {
        if (-not $p) { continue }
        if ($p.TrimEnd('\') -ieq $target) { continue }   # de-dup, we re-add at front
        if ($p.StartsWith(':\')) { $p = 'C' + $p }        # repair known broken drive letter
        $parts += $p
    }
    $new = (@($Node27) + $parts) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $new, $scope)
    L ("[$scope] PATH len " + $cur.Length + " -> " + $new.Length + "; head=" + $Node27)
}
foreach ($scope in @('Machine', 'User')) {
    $back = [Environment]::GetEnvironmentVariable('Path', $scope)
    if (-not $back.StartsWith($Node27)) { L ("[$scope] VERIFY FAILED: " + $back.Substring(0, [Math]::Min(80, $back.Length))); exit 1 }
    L ("[$scope] verify OK")
}
L 'ALL OK (new terminals pick up Node 27 + npm 12.2.1)'
Write-Host '=== Node 27 global PATH OK ==='
exit 0
