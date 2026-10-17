# game_mode.ps1 - self-contained testsigning switch for RAMBlockCache
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}
Write-Host "Current testsigning status:"
bcdedit /enum | Select-String 'testsigning'
Write-Host ""
Write-Host " [1] ON  - enable RAMBlockCache acceleration (reboot required)"
Write-Host " [2] OFF - before playing Valorant / Vanguard games (reboot required)"
$c = Read-Host "Choose 1 or 2"
if ($c -eq '1') {
    bcdedit /set testsigning on
    Write-Host "[ok] testsigning ON. Reboot, then RAMBlockCache works."
} else {
    bcdedit /set testsigning off
    Write-Host "[ok] testsigning OFF. Reboot, then start the game."
}
Read-Host "Press Enter to close"
