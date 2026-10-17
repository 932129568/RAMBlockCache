<#
.SYNOPSIS
  Safe wrapper around ramboostctl with boot-critical guard and confirmation.

.EXAMPLE
  powershell -File ctl_safe.ps1 status
  powershell -File ctl_safe.ps1 attach -Volume F:
  powershell -File ctl_safe.ps1 detach -Volume F: -Yes
#>
#requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Position=0)][ValidateSet('status','attach','detach')]
    [string]$Action = 'status',
    [string]$Volume,
    [switch]$Yes
)
# Error-handling convention (spec D.3): $ErrorActionPreference='Stop' below; errors are
# reported as red Write-Host plus an explicit non-zero exit, matching deploy.ps1's Fail()
# and ramboostsetup.ps1's Die(). Exit codes:
#   0 = success (ramboostctl.exe's own exit code is propagated on attach/detach/status)
#   1 = ramboostctl.exe not found / boot-critical volume refused / user aborted
#   2 = usage error (missing -Volume)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Find-First($cands){ foreach($c in $cands){ if($c -and (Test-Path $c)){ return $c } } return $null }
$ctl = Find-First @(
    (Join-Path $root 'dist\ramboostctl.exe'),
    (Join-Path $root 'ctl\ramboostctl.exe')
)
if(-not $ctl){ Write-Host "[!] ramboostctl.exe not found." -ForegroundColor Red; exit 1 }

function Get-BootCriticalLetters{
    $set = New-Object System.Collections.Generic.HashSet[string]
    [void]$set.Add($env:SystemDrive.Substring(0,1).ToUpper())
    $mm = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
    foreach($n in 'PagingFiles','ExistingPageFiles'){
        foreach($line in @((Get-ItemProperty -Path $mm -Name $n -ErrorAction SilentlyContinue).$n)){
            if($line -match '^\s*([A-Za-z]):'){ [void]$set.Add($matches[1].ToUpper()) }
        }
    }
    $cc = 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
    foreach($n in 'DumpFile','DedicatedDumpFile','MiniDumpDir'){
        $v = (Get-ItemProperty -Path $cc -Name $n -ErrorAction SilentlyContinue).$n
        if($v){
            if($v -match '%'){ [void]$set.Add($env:SystemDrive.Substring(0,1).ToUpper()) }
            elseif($v -match '^\s*([A-Za-z]):'){ [void]$set.Add($matches[1].ToUpper()) }
        }
    }
    return $set
}

switch($Action){
    'status' { & $ctl volumes; & $ctl status; exit $LASTEXITCODE }
    'attach' {
        if(-not $Volume){ Write-Host "[!] -Volume required, e.g. -Volume F:" -ForegroundColor Red; exit 2 }
        $L = $Volume.TrimEnd(':').Substring(0,1).ToUpper()
        $critical = Get-BootCriticalLetters
        if($critical.Contains($L)){
            Write-Host "[!] Volume ${L}: is boot-critical; refusing." -ForegroundColor Red; exit 1
        }
        if(-not $Yes){
            Write-Host "About to attach volume ${L}: to RAMBlockCache (runtime, no reboot)." -ForegroundColor Yellow
            $a = Read-Host "Type the drive letter again to confirm"
            if(($a.TrimEnd(':').Substring(0,1).ToUpper()) -ne $L){ Write-Host "Aborted."; exit 1 }
        }
        & $ctl attach $L
        exit $LASTEXITCODE
    }
    'detach' {
        if(-not $Volume){ Write-Host "[!] -Volume required" -ForegroundColor Red; exit 2 }
        $L = $Volume.TrimEnd(':').Substring(0,1).ToUpper()
        Write-Host "Detaching ${L}: (flushes dirty data first)..." -ForegroundColor Yellow
        & $ctl detach $L
        exit $LASTEXITCODE
    }
}
