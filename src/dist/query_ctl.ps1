# Elevated ctl query -> ctl_query.txt
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}
$d = Split-Path -Parent $PSCommandPath
$ctl = Join-Path $d "ramboostctl.exe"
$out = Join-Path $d "ctl_query.txt"
"=== volumes ===" | Out-File $out -Encoding utf8
& $ctl volumes 2>&1 | Out-File $out -Append -Encoding utf8
"=== status ===" | Out-File $out -Append -Encoding utf8
& $ctl status 2>&1 | Out-File $out -Append -Encoding utf8
"=== get-config ===" | Out-File $out -Append -Encoding utf8
& $ctl get-config 2>&1 | Out-File $out -Append -Encoding utf8
