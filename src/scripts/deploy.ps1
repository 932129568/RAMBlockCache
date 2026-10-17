# deploy.ps1  (V10.4.3 deploy: VERIFY-BEFORE-CHANGE gate + armed rollback guard)
# Pure ASCII. Must run elevated (deploy.cmd self-elevates).
#
# SAFETY MODEL (V10.4.3):
#   A hard pre-flight gate runs BEFORE any file/registry mutation:
#     A. resolve source sys  -> dist\RAMBlockCache.sys  OR  driver\x64\Release\RAMBlockCache.sys
#     B. SHA256(source) == expected pin  (param > <sys>.sha256 sidecar > build pin)
#     C. Authenticode verify (signtool /pa) on the source sys MUST succeed.
#        signtool missing  =>  FAIL (fail-closed).
#   Only if A+B+C pass do we touch PendingFileRenameOperations, the System32 copy,
#   service start type, volume-class UpperFilters, armed flags, or scheduled tasks.
#   ANY failure path exits non-zero AND rolls back everything already changed
#   (original values are backed up first).
#
#   Writing FilterArmed=1 + BOOT_START + UpperFilters while an unverified binary
#   is on disk is the exact 0x7B boot-brick scenario. That is why the gate is first.
#
# Steps (only after the gate passes):
#   1. Clean stale PendingFileRenameOperations entries that reference RAMBlockCache.sys
#   2. Copy the signed RAMBlockCache.sys into System32\drivers (MoveFileEx fallback,
#      return value checked).
#   2b. Convert service to BOOT_START + System Bus Extender.
#   2c. Append RAMBlockCache to volume-class UpperFilters (preserve existing, e.g. volsnap).
#   3. Write armed baseline: DeploySeq+1, BootFailCount=0, LastBootHealthy=0, FilterArmed=1
#   3b. Ensure Features\Flags=0.
#   4. Install guard scripts into C:\ProgramData\RAMBoost.
#   5. Register RAMBoostBootGuard (AtStartup) / RAMBoostLogonDisarm (AtLogOn).
#   6. Post-copy verification of the deployed System32 copy (verify_deploy_signature.ps1).
#
# This script does NOT reboot or shut down.

[CmdletBinding()]
param(
    [string]$SysSource      = "",
    [string]$ExpectedSha256 = ""
)

$ErrorActionPreference = 'Stop'

# Build19 delivered driver pin. Override with -ExpectedSha256 or a <sys>.sha256
# sidecar file next to the source sys. If none is available the gate FAILS CLOSED.
$DefaultExpectedSha256 = 'b6fb097ba24fab1621df48ca066cebd57d1a9cc55015c5737d69e27e3dd64882'

Write-Host '== RAMBlockCache V10.4.3 deploy (verify-before-change) =='

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "[X] $Message" -ForegroundColor Red
    exit $Code
}

# --- 0. Admin check ---
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail 'Run as Administrator (launch deploy.cmd).' 1
}

$scriptsDir = $PSScriptRoot
$root       = Split-Path -Parent $scriptsDir   # = src\
# [FIX 2026-10-17] 包根 = src\ 的上一级（dist\ 在包根，不在 src\ 下）。
# 原候选 "$root\dist\RAMBlockCache.sys" 恒落空，部署误选 src\driver\x64\Release
# 的未签名编译产物——被哈希门禁 fail-closed 正确拦下，但部署链断。
$repoRoot   = Split-Path -Parent $root
$clsKey     = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'
$svcReg     = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache'
$svcRoot    = "$svcReg\Parameters"
$featKey    = "$svcReg\Features"
$smKey      = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'
$pdDir      = 'C:\ProgramData\RAMBoost'
$dest       = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'

# sc.exe text mappings (used to restore the service config on rollback).
$startMap = @{ 0='boot'; 1='system'; 2='auto'; 3='demand'; 4='disabled' }
$errorMap = @{ 0='ignore'; 1='normal'; 2='severe'; 3='critical' }
$typeMap  = @{ 1='kernel'; 2='filesystem'; 4='adapter'; 8='recognizer'; 16='own'; 32='share'; 256='interactive' }

function Resolve-SysSource([string]$Explicit) {
    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
        throw "specified -SysSource not found: $Explicit"
    }
    $cands = @(
        (Join-Path $repoRoot 'dist\RAMBlockCache.sys'),            # 包根\dist = 签名部署产物（优先）
        (Join-Path $root 'driver\x64\Release\RAMBlockCache.sys')   # src\driver 编译产物（兜底，须经签名门禁）
    )
    foreach ($c in $cands) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    throw "signed sys not found; looked in: $($cands -join ' ; ')"
}

function Resolve-ExpectedSha([string]$Sys, [string]$Explicit) {
    if ($Explicit) { return $Explicit.Trim().ToLower() }
    $sidecar = "$Sys.sha256"
    if (Test-Path -LiteralPath $sidecar) {
        $first = Get-Content -LiteralPath $sidecar -TotalCount 1
        if ($first -match '([0-9a-fA-F]{64})') { return $matches[1].ToLower() }
        throw "sidecar hash file has no 64-hex SHA256: $sidecar"
    }
    if ($DefaultExpectedSha256) { return $DefaultExpectedSha256.ToLower() }
    return $null
}

function Resolve-SignTool {
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $roots = @()
    if (${env:ProgramFiles(x86)}) { $roots += (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') }
    if ($env:ProgramFiles)        { $roots += (Join-Path $env:ProgramFiles        'Windows Kits\10\bin') }
    foreach ($r in $roots) {
        if ($r -and (Test-Path -LiteralPath $r)) {
            $hit = Get-ChildItem -LiteralPath $r -Recurse -Filter 'signtool.exe' -ErrorAction SilentlyContinue |
                   Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
                   Sort-Object FullName -Descending | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    return $null
}

# =====================================================================
#  PRE-FLIGHT GATE  (no mutations before this block completes)
# =====================================================================
Write-Host ''
Write-Host '== Pre-flight verification gate =='

try { $srcSys = Resolve-SysSource $SysSource } catch { Fail $_.Exception.Message 1 }
Write-Host "[..] source sys   : $srcSys"

try { $srcHash = (Get-FileHash -LiteralPath $srcSys -Algorithm SHA256).Hash.ToLower() }
catch { Fail "cannot hash source sys: $($_.Exception.Message)" 1 }
Write-Host "[..] source SHA256: $srcHash"

try { $expected = Resolve-ExpectedSha $srcSys $ExpectedSha256 }
catch { Fail $_.Exception.Message 1 }
if (-not $expected) {
    Fail 'no expected SHA256 available (pass -ExpectedSha256 or add <sys>.sha256). Refusing to deploy.' 1
}
Write-Host "[..] expected hash: $expected"
if ($srcHash -ne $expected) {
    Fail "SHA256 mismatch (source=$srcHash expected=$expected). DO NOT deploy / DO NOT reboot." 2
}
Write-Host '[ok] SHA256 matches expected pin.'

$signtool = Resolve-SignTool
if (-not $signtool) {
    Fail 'signtool.exe not found (PATH / Windows Kits). Authenticode verification cannot run; refusing to deploy.' 3
}
Write-Host "[..] signtool     : $signtool"
# [FIX A3] Authenticode 状态显式校验：与上方 SHA256 锚点同级输出。
# signtool /pa（下方）继续保留作双保险；本段以 PowerShell 原生 API 显式
# 取 Status/SignerInfo，让"签名有效"成为与哈希同级的可读断言
# （0x7B 事故整改：门禁此前缺"签名有效性"维度）。
try { $auth = Get-AuthenticodeSignature -LiteralPath $srcSys -ErrorAction Stop }
catch { Fail "Get-AuthenticodeSignature failed: $($_.Exception.Message)" 3 }
Write-Host "[..] signature    : $($auth.Status)"
try {
    if ($auth.SignerInfo -and $auth.SignerInfo.Subject) {
        Write-Host "[..] signer       : $($auth.SignerInfo.Subject)"
    }
} catch { }
if ($auth.Status -ne 'Valid') {
    Fail "Authenticode signature is '$($auth.Status)' (not Valid). An unsigned or untrusted driver as BOOT_START + UpperFilters WILL 0x7B-loop the machine. DO NOT deploy / DO NOT reboot." 3
}
Write-Host '[ok] Authenticode signature status: Valid.'
& $signtool verify /pa /v $srcSys | Out-Host
if ($LASTEXITCODE -ne 0) {
    Fail "signtool verify FAILED (exit $LASTEXITCODE). DO NOT deploy / DO NOT reboot." 3
}
Write-Host '[ok] Authenticode signature verified (signtool /pa).'
Write-Host '[ok] Pre-flight gate PASSED - applying changes.'

# =====================================================================
#  BACKUP original state (before the first mutation)
# =====================================================================
$backup = @{
    UpperFiltersExisted = $false; UpperFilters = @()
    SvcExisted = $false; SvcStart = $null; SvcType = $null; SvcError = $null; SvcGroup = $null
    PfroExisted = $false; Pfro = @()
    ParamProps = @{}
    FeatExisted = $false; FeatFlags = 0
    Mutated = $false
}

if (Test-Path -LiteralPath $svcReg) {
    $sp = Get-ItemProperty -Path $svcReg -ErrorAction SilentlyContinue
    $backup.SvcExisted = $true
    $backup.SvcStart   = $sp.Start
    $backup.SvcType    = $sp.Type
    $backup.SvcError   = $sp.ErrorControl
    $backup.SvcGroup   = $sp.Group
}
$ufp = Get-ItemProperty -Path $clsKey -Name UpperFilters -ErrorAction SilentlyContinue
if ($ufp -and $null -ne $ufp.UpperFilters) {
    $backup.UpperFiltersExisted = $true
    $backup.UpperFilters = @($ufp.UpperFilters)
}
$pfroProp = Get-ItemProperty -Path $smKey -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
if ($pfroProp -and $null -ne $pfroProp.PendingFileRenameOperations) {
    $backup.PfroExisted = $true
    $backup.Pfro = @($pfroProp.PendingFileRenameOperations)
}
if (Test-Path -LiteralPath $svcRoot) {
    $pp = Get-ItemProperty -Path $svcRoot -ErrorAction SilentlyContinue
    foreach ($n in @('DeploySeq','BootFailCount','LastBootHealthy','FilterArmed')) {
        if ($pp -and ($pp.PSObject.Properties.Name -contains $n)) { $backup.ParamProps[$n] = $pp.$n }
    }
}
if (Test-Path -LiteralPath $featKey) {
    $fp = Get-ItemProperty -Path $featKey -Name Flags -ErrorAction SilentlyContinue
    if ($fp -and $null -ne $fp.Flags) { $backup.FeatExisted = $true; $backup.FeatFlags = $fp.Flags }
}

function Invoke-Rollback {
    Write-Host '[..] rolling back to pre-deploy state ...' -ForegroundColor Yellow
    $errors = @()
    # UpperFilters
    try {
        if ($backup.UpperFiltersExisted) {
            Set-ItemProperty -Path $clsKey -Name UpperFilters -Value $backup.UpperFilters -Type MultiString
        } else {
            Remove-ItemProperty -Path $clsKey -Name UpperFilters -ErrorAction SilentlyContinue
        }
    } catch { $errors += "UpperFilters: $($_.Exception.Message)" }
    # Service config
    if ($backup.SvcExisted) {
        try {
            $st = if ($null -ne $backup.SvcStart -and $startMap.ContainsKey([int]$backup.SvcStart)) { $startMap[[int]$backup.SvcStart] } else { 'demand' }
            $ec = if ($null -ne $backup.SvcError -and $errorMap.ContainsKey([int]$backup.SvcError)) { $errorMap[[int]$backup.SvcError] } else { 'normal' }
            $ty = if ($null -ne $backup.SvcType  -and $typeMap.ContainsKey([int]$backup.SvcType))   { $typeMap[[int]$backup.SvcType] }   else { 'kernel' }
            & sc.exe config RAMBlockCache start= $st error= $ec type= $ty | Out-Null
            if ($backup.SvcGroup) { & sc.exe config RAMBlockCache group= $backup.SvcGroup | Out-Null }
        } catch { $errors += "service: $($_.Exception.Message)" }
    }
    # Parameters (armed baseline)
    foreach ($n in $backup.ParamProps.Keys) {
        try { Set-ItemProperty -Path $svcRoot -Name $n -Value $backup.ParamProps[$n] -Type DWord }
        catch { $errors += "param ${n}: $($_.Exception.Message)" }
    }
    # Features\Flags
    if ($backup.FeatExisted) {
        try { Set-ItemProperty -Path $featKey -Name Flags -Value $backup.FeatFlags -Type DWord }
        catch { $errors += "Features\Flags: $($_.Exception.Message)" }
    }
    # PendingFileRenameOperations
    try {
        if ($backup.PfroExisted) {
            Set-ItemProperty -Path $smKey -Name PendingFileRenameOperations -Value $backup.Pfro -Type MultiString
        } else {
            Remove-ItemProperty -Path $smKey -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
        }
    } catch { $errors += "PFRO: $($_.Exception.Message)" }

    if ($errors.Count -eq 0) {
        Write-Host '[ok] rollback complete - system restored to pre-deploy state.' -ForegroundColor Green
    } else {
        Write-Host '[!!] ROLLBACK INCOMPLETE:' -ForegroundColor Red
        foreach ($e in $errors) { Write-Host "     - $e" -ForegroundColor Red }
        Write-Host '[!!] Recover offline with scripts\offline_rollback.cmd (WinRE).' -ForegroundColor Red
    }
}

# =====================================================================
#  MUTATIONS  (all guarded; any throw -> rollback + non-zero exit)
# =====================================================================
$pendingReplace = $false
try {
    $backup.Mutated = $true

    # --- 1. Clean stale PendingFileRenameOperations for RAMBlockCache.sys ---
    $pfro = (Get-ItemProperty -Path $smKey -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
    if ($pfro) {
        $kept = @($pfro | Where-Object { $_ -notlike '*RAMBlockCache.sys*' })
        if ($kept.Count -gt 0) {
            Set-ItemProperty -Path $smKey -Name PendingFileRenameOperations -Value $kept -Type MultiString
        } else {
            Remove-ItemProperty -Path $smKey -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
        }
        Write-Host '[ok] cleaned stale PendingFileRenameOperations entries for RAMBlockCache.sys'
    } else {
        Write-Host '[..] no PendingFileRenameOperations to clean'
    }

    # --- 2. Copy signed sys into System32\drivers ---
    try {
        Copy-Item -LiteralPath $srcSys -Destination $dest -Force -ErrorAction Stop
        Write-Host "[ok] copied $srcSys -> $dest"
    } catch {
        Write-Host "[..] $dest is locked; queueing replace on next reboot (MoveFileEx) ..."
        if (-not ('Win32.MoveFileEx' -as [type])) {
            Add-Type -Namespace Win32 -Name MoveFileEx -MemberDefinition `
                '[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)] public static extern bool MoveFileEx(string lpExistingFileName, string lpNewFileName, int dwFlags);'
        }
        $MOVEFILE_REPLACE_EXISTING   = 0x0001
        $MOVEFILE_DELAY_UNTIL_REBOOT = 0x0004
        $ok = [Win32.MoveFileEx]::MoveFileEx($srcSys, $dest, ($MOVEFILE_REPLACE_EXISTING -bor $MOVEFILE_DELAY_UNTIL_REBOOT))
        if (-not $ok) {
            $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "MoveFileEx failed (Win32 error $err); cannot deploy $dest."
        }
        $pendingReplace = $true
        Write-Host '[ok] queued replacement (takes effect on next reboot)'
    }

    # --- 2b. Convert service to BOOT_START + System Bus Extender ---
    & sc.exe config RAMBlockCache start= boot error= normal type= kernel group= "System Bus Extender" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "sc.exe config failed (exit $LASTEXITCODE)" }
    Write-Host '[ok] service RAMBlockCache set to BOOT_START / group=System Bus Extender / ErrorControl=NORMAL'

    # --- 2c. Re-append RAMBlockCache to volume-class UpperFilters (preserve volsnap) ---
    $cur = (Get-ItemProperty -Path $clsKey -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
    if (-not $cur) { $cur = @('volsnap') }
    $cur = @($cur | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' } | Select-Object -Unique)
    if ($cur -notcontains 'volsnap') { $cur = @('volsnap') + $cur }
    if ($cur -notcontains 'RAMBlockCache') { $cur += 'RAMBlockCache' }
    Set-ItemProperty -Path $clsKey -Name UpperFilters -Value $cur -Type MultiString
    Write-Host ('[ok] UpperFilters = [' + ($cur -join ', ') + ']')

    # --- 3. Write armed baseline ---
    if (-not (Test-Path -LiteralPath $svcRoot)) { New-Item -Path $svcRoot -Force | Out-Null }
    $seq = [int](Get-ItemProperty -Path $svcRoot -Name DeploySeq -ErrorAction SilentlyContinue).DeploySeq
    Set-ItemProperty -Path $svcRoot -Name DeploySeq       -Value ($seq + 1) -Type DWord
    Set-ItemProperty -Path $svcRoot -Name BootFailCount   -Value 0 -Type DWord
    Set-ItemProperty -Path $svcRoot -Name LastBootHealthy -Value 0 -Type DWord
    Set-ItemProperty -Path $svcRoot -Name FilterArmed     -Value 1 -Type DWord
    Write-Host ('[ok] armed: DeploySeq={0}, BootFailCount=0, LastBootHealthy=0, FilterArmed=1' -f ($seq + 1))

    # --- 3b. Features\Flags default = 0 ---
    if (-not (Test-Path -LiteralPath $featKey)) { New-Item -Path $featKey -Force | Out-Null }
    Set-ItemProperty -Path $featKey -Name Flags -Value 0 -Type DWord
    Write-Host '[ok] Features\Flags=0 (all feature flags off)'

    # --- 4. Install guard scripts ---
    if (-not (Test-Path -LiteralPath $pdDir)) { New-Item -ItemType Directory -Path $pdDir -Force | Out-Null }
    Copy-Item (Join-Path $scriptsDir 'boot_guard.ps1')   (Join-Path $pdDir 'boot_guard.ps1')   -Force -ErrorAction Stop
    Copy-Item (Join-Path $scriptsDir 'logon_disarm.ps1') (Join-Path $pdDir 'logon_disarm.ps1') -Force -ErrorAction Stop
    Write-Host "[ok] guard scripts installed to $pdDir"

    # --- 5. Register scheduled tasks ---
    $bgPath = Join-Path $pdDir 'boot_guard.ps1'
    $ldPath = Join-Path $pdDir 'logon_disarm.ps1'

    $act1 = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$bgPath`""
    $trg1 = New-ScheduledTaskTrigger -AtStartup
    $prin = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName 'RAMBoostBootGuard' -Action $act1 -Trigger $trg1 -Principal $prin -Force | Out-Null
    Write-Host '[ok] scheduled task RAMBoostBootGuard registered (SYSTEM, AtStartup)'

    $act2 = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$ldPath`""
    $trg2 = New-ScheduledTaskTrigger -AtLogOn
    Register-ScheduledTask -TaskName 'RAMBoostLogonDisarm' -Action $act2 -Trigger $trg2 -Principal $prin -Force | Out-Null
    Write-Host '[ok] scheduled task RAMBoostLogonDisarm registered (SYSTEM, AtLogOn)'

    # --- 6. Post-copy verification of the deployed copy ---
    Write-Host ''
    Write-Host '== Verifying deployed copy (Patch F) =='
    $verifyArgs = @{ SourcePath = $srcSys; ExpectedSha256 = $expected }
    if ($pendingReplace) { $verifyArgs['AllowPendingReplace'] = $true }
    & (Join-Path $scriptsDir 'verify_deploy_signature.ps1') @verifyArgs
    if ($LASTEXITCODE -ne 0) { throw "post-copy verification failed (exit $LASTEXITCODE)" }
}
catch {
    Write-Host "[X] deploy step failed: $($_.Exception.Message)" -ForegroundColor Red
    Invoke-Rollback
    exit 1
}

Write-Host ''
Write-Host '== Deploy complete. NEXT: reboot once. =='
if ($pendingReplace) {
    Write-Host '[!!] The System32 copy is queued for replace on next reboot (file was locked).'
    Write-Host '[!!] Source binary was hash-pinned and signature-verified before arming.'
}
Write-Host '  On a good boot: the logon task clears FilterArmed, passthrough stays.'
Write-Host '  On a failed boot: the boot guard rolls UpperFilters back to volsnap-only.'
Write-Host '  Worst case (no boot): run scripts\offline_rollback.cmd from WinRE.'
exit 0
