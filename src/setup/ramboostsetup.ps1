<#
.SYNOPSIS
  RAMBlockCache V9 safe installer (service-based, runtime volume attach).
  [P1-8] 这是【备选/历史】安装模型，不是默认入口。

.DESCRIPTION
  V9 (post 0x7B) safety model:
    * NEVER registers any Volume-class UpperFilters -> the filter is never
      auto-attached by PnP to the boot volume.
    * Service is DEMAND_START. DriverEntry only creates the control device.
    * Volumes are attached at runtime via IOCTL (no reboot), and only if the
      user explicitly lists them in -DataVolumes.
    * Backs up UpperFilters before any change; auto-rollback on failure.

  [P1-8] 权威入口说明（两套语义并存，以此段为准）：
    默认/权威安装入口 = src\scripts\install.cmd -> install.ps1
      （编译 -> 签名 -> 注册服务 -> Volume UpperFilters + BOOT_START 挂栈模型，
        与 deploy.ps1 / verify_deploy_signature.ps1 的 SHA256 门禁同一套）。
    本脚本（ramboostsetup.ps1）= V9"零 UpperFilters、运行期 IOCTL attach"
      备选模型：适合不允许引导期挂栈的保守场景，但默认缓存不覆盖开机过程。
    二者语义互斥，请只选其一；混用会导致"以为在缓存、实际没挂栈"或相反。

.PARAMETER DataVolumes
  Optional drive letters to attach after install, e.g. "F:","D:". Default none.

.PARAMETER CleanupLegacy
  Remove a previously (V8) installed "RAMBlockCache" UpperFilters entry.

.PARAMETER Rollback
  Restore UpperFilters from backup, stop/delete the service and remove the file.

.PARAMETER EnableTestSigning
  Turn testsigning ON (requires a reboot before the driver can load).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ramboostsetup.ps1
  powershell -ExecutionPolicy Bypass -File ramboostsetup.ps1 -DataVolumes F:,D:
#>
#requires -RunAsAdministrator
[CmdletBinding()]
param(
    [string[]]$DataVolumes = @(),
    [switch]$CleanupLegacy,
    [switch]$Rollback,
    [switch]$EnableTestSigning,
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'
$svc     = 'RAMBlockCache'
$volCls  = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'
$bakFile = Join-Path $PSScriptRoot 'UpperFilters.backup.json'
$root    = Split-Path -Parent $PSScriptRoot

function Write-Step($m){ Write-Host "[*] $m" -ForegroundColor Cyan }
function Write-Ok($m){ Write-Host "[+] $m" -ForegroundColor Green }
function Die($m){ Write-Host "[!] $m" -ForegroundColor Red; exit 1 }

function Find-First($candidates){
    foreach($c in $candidates){ if($c -and (Test-Path $c)){ return $c } }
    return $null
}

function Get-UpperFilters{
    $v = (Get-ItemProperty -Path $volCls -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
    if($null -eq $v){ return @() } else { return @($v) }
}
function Backup-UpperFilters{
    $cur = Get-UpperFilters
    [pscustomobject]@{ time=(Get-Date).ToString('s'); filters=$cur } |
        ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $bakFile
}
function Restore-UpperFilters{
    if(Test-Path $bakFile){
        $j = Get-Content -Raw -Path $bakFile | ConvertFrom-Json
        Set-ItemProperty -Path $volCls -Name UpperFilters -Value ([string[]]$j.filters) -Type MultiString
        Write-Ok "UpperFilters restored from backup"
    }
}

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

# ---------------- Rollback mode ----------------
if($Rollback){
    Write-Step "Rolling back"
    if(Get-Service -Name $svc -ErrorAction SilentlyContinue){ sc.exe stop $svc | Out-Null }
    Restore-UpperFilters
    sc.exe delete $svc | Out-Null
    $dst = Join-Path $env:WINDIR 'System32\drivers\RAMBlockCache.sys'
    if(Test-Path $dst){ Remove-Item -Force $dst }
    Write-Ok "Rollback complete"
    exit 0
}

# ---------------- Preflight ----------------
$sysSrc = Find-First @(
    (Join-Path $root 'dist\RAMBlockCache.sys'),
    (Join-Path $root 'driver\x64\Release\RAMBlockCache.sys'),
    (Join-Path $root 'driver\x64\Debug\RAMBlockCache.sys')
)
$ctl = Find-First @(
    (Join-Path $root 'dist\ramboostctl.exe'),
    (Join-Path $root 'ctl\ramboostctl.exe')
)
if(-not $sysSrc){ Die "RAMBlockCache.sys not found. Build the driver first." }

# Testsigning
$bcd = (& bcdedit /enum '{current}') -join "`n"
if($EnableTestSigning){
    bcdedit /set testsigning on | Out-Null
    Write-Ok "testsigning enabled; REBOOT required before loading the driver"
} elseif($bcd -notmatch '(?m)testsigning\s+Yes'){
    Write-Host "[!] testsigning is OFF. Re-run with -EnableTestSigning and reboot." -ForegroundColor Yellow
}

$critical = Get-BootCriticalLetters
foreach($d in $DataVolumes){
    $L = $d.TrimEnd(':').Substring(0,1).ToUpper()
    if($critical.Contains($L)){ Die "Volume ${L}: is boot-critical (page/dump/hibernate); refusing to attach." }
}

$createdSvc = $false
try{
    Backup-UpperFilters
    Write-Ok "UpperFilters backed up -> $bakFile"

    if($CleanupLegacy){
        $cur = Get-UpperFilters
        $new = @($cur | Where-Object { $_ -and $_ -ne $svc })
        if($cur.Count -ne $new.Count){
            Set-ItemProperty -Path $volCls -Name UpperFilters -Value ([string[]]$new) -Type MultiString
            Write-Ok "Legacy '$svc' UpperFilters entry removed"
        }
    }

    # Copy driver
    $dst = Join-Path $env:WINDIR 'System32\drivers\RAMBlockCache.sys'
    Copy-Item -Force -Path $sysSrc -Destination $dst
    Write-Ok "Driver copied"

    # Service (demand start)
    $existing = Get-Service -Name $svc -ErrorAction SilentlyContinue
    if(-not $existing){
        New-Service -Name $svc -BinaryPathName $dst -StartupType Manual -DisplayName 'RAMBlockCache' | Out-Null
        $createdSvc = $true
        Write-Ok "Service created (DEMAND_START)"
    } else {
        sc.exe config $svc type= kernel start= demand binPath= $dst | Out-Null
        Write-Ok "Service already present; config refreshed"
    }

    if(-not $NoStart){
        sc.exe start $svc | Out-Null
        Start-Sleep -Milliseconds 800
        if($ctl){
            $ver = & $ctl version 2>&1
            if($LASTEXITCODE -ne 0){ throw "control channel not responding" }
            Write-Ok "Control channel live"
        }
    }

    # Attach data volumes at runtime (no reboot)
    foreach($d in $DataVolumes){
        $L = $d.TrimEnd(':').Substring(0,1).ToUpper()
        Write-Step "Attaching volume ${L}:"
        if($ctl){ & $ctl attach $L | Out-Host; if($LASTEXITCODE -ne 0){ throw "attach $L failed" } }
    }

    Write-Ok "Install complete. No UpperFilters were added; boot stack untouched."
    if($DataVolumes.Count -eq 0){
        Write-Host "    No volumes attached. Attach later: ramboostctl attach F  (no reboot)" -ForegroundColor DarkGray
    }
}
catch{
    Write-Host "[!] Install failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Step "Auto-rollback"
    sc.exe stop $svc | Out-Null
    if($createdSvc){ sc.exe delete $svc | Out-Null }
    Restore-UpperFilters
    Die "Rolled back. System left in a safe state."
}
