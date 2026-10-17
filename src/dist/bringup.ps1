# Bring-up: safe-mode off, reenumerate, attach F/D, status
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}
[Console]::OutputEncoding = [Text.Encoding]::GetEncoding(936)
$d = Split-Path -Parent $PSCommandPath
$ctl = Join-Path $d "ramboostctl.exe"
$log = Join-Path $d "bringup.log"
if (Test-Path $log) { Remove-Item $log }
function Run($tag, $a) {
    Add-Content $log "=== $tag ===" -Encoding UTF8
    (& $ctl @a 2>&1) | Out-File $log -Append -Encoding UTF8
}

Run "safe-mode off" @("safe-mode","off")

$cs = @"
using System;
using System.Runtime.InteropServices;
public static class CfgM {
  [DllImport("cfgmgr32.dll")] public static extern int CM_Locate_DevNode(out uint dn, IntPtr p, uint f);
  [DllImport("cfgmgr32.dll")] public static extern int CM_Reenumerate_DevNode(uint dn, uint f);
}
"@
Add-Type -TypeDefinition $cs
$dn = [uint32]0
$r1 = [CfgM]::CM_Locate_DevNode([ref]$dn, [IntPtr]::Zero, 0)
$r2 = [CfgM]::CM_Reenumerate_DevNode($dn, 0)
Add-Content $log "=== reenumerate locate=$r1 reenum=$r2 ===" -Encoding UTF8
Start-Sleep 7

Run "volumes" @("volumes")
Run "attach F" @("attach","F")
Run "attach D" @("attach","D")
Run "status" @("status")
Add-Content $log "BRINGUP_DONE" -Encoding UTF8
