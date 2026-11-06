# Check deploy preconditions: PFRO pending + driver file lock state
$ErrorActionPreference = 'Continue'

Write-Host '== PendingFileRenameOperations =='
$sm = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'
$v = (Get-ItemProperty $sm -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
if ($v) { $v | ForEach-Object { Write-Host "  $_" } } else { Write-Host '  (none pending)' }

Write-Host '== System32 driver file lock test =='
$p = 'C:\Windows\System32\drivers\RAMBlockCache.sys'
try {
    $fs = [System.IO.File]::Open($p, 'Open', 'ReadWrite', 'None')
    $fs.Close()
    Write-Host '  UNLOCKED (can overwrite in place)'
} catch {
    Write-Host ('  LOCKED: ' + $_.Exception.GetType().Name)
}

Write-Host '== Running driver first 8 sha (running instance identity) =='
(Get-FileHash $p -Algorithm SHA256).Hash.ToLower().Substring(0,8)
