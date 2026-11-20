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
$DefaultExpectedSha256 = '3c696e140f6ef8a045ed6bcc391298d16f17d57b0929d4282eda3759c08c6fd0'

Write-Host '== RAMBlockCache V10.4.3 deploy (verify-before-change) =='
# [FIX G1 2026-11-15] 记录引擎身份：门禁失败时必须能区分"文件不对"和"宿主编译器退化"。
# 现场出现过 Get-FileHash 不可解析的宿主（Utility 模块未提供该 cmdlet），
# 只看单行报错无法判断是哪一类。PSEdition 在 5.1 为 Desktop，旧宿主可能为空。
$hostEngine = 'PowerShell ' + $PSVersionTable.PSVersion.ToString()
if ($PSVersionTable.PSEdition) { $hostEngine = $hostEngine + ' / ' + $PSVersionTable.PSEdition }
Write-Host ("[..] host engine  : {0}" -f $hostEngine)

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
        # [FIX S-02] sidecar 首行哈希正则三处统一（deploy.ps1 / verify_deploy_signature.ps1 /
        # check_signature.ps1）：词边界匹配，禁行内任意位置取首条——"SHA256: <hash>" 格式
        # 必须三处同解；改此正则必须同步另两处。
        if ($first -match '(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])') { return $matches[0].ToLower() }
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

# [FIX G1 2026-11-15] Cmdlet-free SHA256.
# 现场故障：某个宿主里 Get-FileHash 报 "is not recognized as the name of a cmdlet"，
# 门禁于是在第一行就 fail-closed（行为正确，但原因错了——文件完全可读）。哈希是硬门禁，
# 不能依赖某个模块被自动加载，故改为直接用 .NET API 计算。
# FileShare.ReadWrite 是必须的：目标文件可能正被驱动持有或被 PendingFileRename 排住，
# 用 FileShare.Read 打开会在"最需要哈希"的那些文件上抛共享冲突。
function Get-Sha256Hex([string]$Path) {
    $fs = [System.IO.File]::Open($Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::ReadWrite)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $bytes = $sha.ComputeHash($fs)
            return ([System.BitConverter]::ToString($bytes) -replace '-', '').ToLowerInvariant()
        } finally { $sha.Dispose() }
    } finally { $fs.Dispose() }
}

# =====================================================================
#  PRE-FLIGHT GATE  (no mutations before this block completes)
# =====================================================================
Write-Host ''
Write-Host '== Pre-flight verification gate =='

try { $srcSys = Resolve-SysSource $SysSource } catch { Fail $_.Exception.Message 1 }
Write-Host "[..] source sys   : $srcSys"

try { $srcHash = Get-Sha256Hex $srcSys }
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
# [FIX G1 2026-11-15] 若该宿主没有 Get-AuthenticodeSignature，不因此放行也不因此中断：
# 权威判据是下方强制执行的 signtool verify /pa（缺 signtool 即 FAIL）。cmdlet 在时
# 仍做显式 Status=Valid 断言，行为与修复前完全一致；不在时打一行说明再交给 signtool。
if (Get-Command Get-AuthenticodeSignature -ErrorAction SilentlyContinue) {
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
} else {
    Write-Host '[..] signature    : cmdlet unavailable on this host -> signtool /pa is the authoritative check below'
}
& $signtool verify /pa /v $srcSys | Out-Host
if ($LASTEXITCODE -ne 0) {
    Fail "signtool verify FAILED (exit $LASTEXITCODE). DO NOT deploy / DO NOT reboot." 3
}
Write-Host '[ok] Authenticode signature verified (signtool /pa).'

# ======================================================================
#  STACK FRAME BUDGET GATE  [298d 2026-11-19]
# ======================================================================
# WHY: the two 0x7F / Arg1=8 bugchecks came from a 1480 B union frame in
# RbDispatchDeviceControl (/O2 inlined every RbMsg* handler; 1304 B of it was
# one RB_STATS local) while the failing path (call RbForward) needed no locals.
# The Authenticode check above CANNOT catch that class: the crashing binary in
# dist\ is itself Valid-signed, and deploy.ps1 had zero frame checks -- so a
# six-step all-[ok] run would happily replace System32 with the crashing image.
# Signature proves WHO built it, not that it still fits in a 12 KB kernel stack.
$frameGate = Join-Path $PSScriptRoot 'check_stack_frame_budget.py'
if (-not (Test-Path -LiteralPath $frameGate)) {
    Fail "stack frame gate missing: $frameGate. Refusing to deploy a binary whose kernel stack frame budget was never checked." 5
}
# No S5 flags here, both deliberate:
#   * $srcSys IS dist\ by default, so S5 would compare the file with itself.
#     That self-comparison is now detected automatically (samefile) and reported
#     as [SKIP] -- it is never reported as [ok]. There used to be a
#     --no-dist-check switch to say this out loud, but it could override
#     --require-dist-match on the same command line (QA F1): a soft flag silently
#     disabling the hard one. Deleted (298f). Callers no longer declare it.
#   * (no --require-dist-match) : $srcSys may legitimately differ from x64\Release
#     (rebuilt on another host). What decides THIS deploy is the frame budget of
#     $srcSys itself. The dist<->Release binding is closed separately, after
#     signing, by RB-sign-driver.cmd with --require-dist-match.
$frameLog = Join-Path $env:TEMP 'rbc-deploy-frame.log'
& python $frameGate --sys $srcSys *> $frameLog
$frameRc = $LASTEXITCODE
if ($frameRc -ne 0) {
    Get-Content -LiteralPath $frameLog -ErrorAction SilentlyContinue | Select-Object -Last 25 | Out-Host
    Fail "STACK FRAME BUDGET VIOLATED (exit $frameRc) on $srcSys. A frame in [1400,1600] B, or a dispatch entry frame over 230 B, means the 0x7F double-fault surface is back. DO NOT deploy / DO NOT reboot." 5
}
Get-Content -LiteralPath $frameLog -ErrorAction SilentlyContinue | Select-String -Pattern 'rsp_alloc|ok\]' | Select-Object -First 3 | Out-Host
Write-Host '[ok] Stack frame budget holds (dispatch <= 230 B, no frame in [1400,1600]).'
Write-Host '[ok] Pre-flight gate PASSED - applying changes.'

# =====================================================================
#  BACKUP original state (before the first mutation)
# =====================================================================
# [FIX S-01] 备份段扩充：除原"值备份"外，新增整键/文件/目录/任务存在性快照
# （fresh-install 回滚"不存在->删除"的依据）。System32 旧文件与守卫脚本的
# 内容备份不在此段——变异可能中途失败，内容备份必须紧贴覆盖点（见变异段）。
$backupErrors = @()
$backup = @{
    UpperFiltersExisted = $false; UpperFilters = @()
    SvcExisted = $false; SvcStart = $null; SvcType = $null; SvcError = $null; SvcGroup = $null
    PfroExisted = $false; Pfro = @()
    ParamProps = @{}
    FeatExisted = $false; FeatFlags = 0
    SvcRootExisted = $false
    FeatKeyExisted = $false
    Sys32Existed = $false; Sys32Backup = $null; Sys32Replaced = $false
    PdDirExisted = $false
    GuardExisted = @{}; GuardBackup = @{}
    TaskExisted = @{}; TaskDef = @{}
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
    foreach ($n in @('DeploySeq','DeployAck','BootFailCount','LastBootHealthy','FilterArmed','TestSigned','Signer','BootReachedUser')) {
        if ($pp -and ($pp.PSObject.Properties.Name -contains $n)) { $backup.ParamProps[$n] = $pp.$n }
    }
}
if (Test-Path -LiteralPath $featKey) {
    $fp = Get-ItemProperty -Path $featKey -Name Flags -ErrorAction SilentlyContinue
    if ($fp -and $null -ne $fp.Flags) { $backup.FeatExisted = $true; $backup.FeatFlags = $fp.Flags }
}
# [FIX S-01] 存在性快照：Parameters/Features 整键、System32 目标、pdDir、
# 两个守卫脚本、两个计划任务（含 XML 导出，供 redeploy 场景还原）。
$backup.SvcRootExisted = Test-Path -LiteralPath $svcRoot
$backup.FeatKeyExisted = Test-Path -LiteralPath $featKey
$backup.Sys32Existed   = Test-Path -LiteralPath $dest
$backup.PdDirExisted   = Test-Path -LiteralPath $pdDir
foreach ($g in @('boot_guard.ps1','logon_disarm.ps1')) {
    $backup.GuardExisted[$g] = Test-Path -LiteralPath (Join-Path $pdDir $g)
}
foreach ($tn in @('RAMBoostBootGuard','RAMBoostLogonDisarm')) {
    $t = Get-ScheduledTask -TaskName $tn -ErrorAction SilentlyContinue
    $backup.TaskExisted[$tn] = [bool]$t
    if ($t) {
        try { $backup.TaskDef[$tn] = Export-ScheduledTask -TaskName $tn }
        catch { $backupErrors += "export task $tn`: $($_.Exception.Message)" }
    }
}
if ($backupErrors.Count -gt 0) {
    foreach ($e in $backupErrors) { Write-Host "[X] backup incomplete: $e" -ForegroundColor Red }
    Fail 'pre-deploy backup incomplete; refusing to mutate (rollback would not be symmetric).' 1
}

function Invoke-Rollback {
    Write-Host '[..] rolling back to pre-deploy state ...' -ForegroundColor Yellow
    $errors = @()
    # ① UpperFilters：存在→恢复 / 不存在→删属性
    try {
        if ($backup.UpperFiltersExisted) {
            Set-ItemProperty -Path $clsKey -Name UpperFilters -Value $backup.UpperFilters -Type MultiString
        } else {
            Remove-ItemProperty -Path $clsKey -Name UpperFilters -ErrorAction SilentlyContinue
        }
    } catch { $errors += "UpperFilters: $($_.Exception.Message)" }
    # ② 服务：存在→sc config 恢复（[FIX S-01] 查退出码）/ 不存在→Stop + sc delete
    #    （fresh-install 残留必须清除，否则机器带着 BOOT_START 服务重启）
    if ($backup.SvcExisted) {
        try {
            $st = if ($null -ne $backup.SvcStart -and $startMap.ContainsKey([int]$backup.SvcStart)) { $startMap[[int]$backup.SvcStart] } else { 'demand' }
            $ec = if ($null -ne $backup.SvcError -and $errorMap.ContainsKey([int]$backup.SvcError)) { $errorMap[[int]$backup.SvcError] } else { 'normal' }
            $ty = if ($null -ne $backup.SvcType  -and $typeMap.ContainsKey([int]$backup.SvcType))   { $typeMap[[int]$backup.SvcType] }   else { 'kernel' }
            & sc.exe config RAMBlockCache start= $st error= $ec type= $ty | Out-Null
            # [FIX S-01] 恢复路径同样查退出码：sc.exe 失败必须可见，不能静默"恢复成功"
            if ($LASTEXITCODE -ne 0) { throw "sc.exe config restore failed (exit $LASTEXITCODE)" }
            # [FIX 建议-2 2026-10-18] 组名含空格（System Bus Extender）需整参数传递，否则 sc.exe 拆词回滚失败
            if ($backup.SvcGroup) {
                & sc.exe config RAMBlockCache "group=$($backup.SvcGroup)" | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "sc.exe config group restore failed (exit $LASTEXITCODE)" }
            }
        } catch { $errors += "service: $($_.Exception.Message)" }
    } else {
        # [FIX S-01] fresh-install：服务是本次部署引入的，停掉并删除。
        # A2：Stop-Service + 10s 有界轮询；轮询超时仍继续 sc.exe delete（打删除标记），
        # 与 install.ps1:53 一致容忍 {0,1060}（1060=服务已不存在）。
        try {
            Stop-Service -Name 'RAMBlockCache' -ErrorAction SilentlyContinue
            $waited = 0
            while ($waited -lt 10) {
                $svc = Get-Service -Name 'RAMBlockCache' -ErrorAction SilentlyContinue
                if (-not $svc -or $svc.Status -eq 'Stopped') { break }
                Start-Sleep -Seconds 1
                $waited++
            }
            & sc.exe delete RAMBlockCache | Out-Null
            if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1060) {
                throw "sc.exe delete failed (exit $LASTEXITCODE; 1060=already-gone is tolerated)"
            }
            Write-Host '[ok] fresh-install service RAMBlockCache removed (stop + sc delete)'
        } catch { $errors += "service delete: $($_.Exception.Message)" }
    }
    # ③ Parameters（armed 基线）：svcRoot 键不存在→整键删 / 存在→恢复备份值 + 删部署新建属性
    try {
        if (-not $backup.SvcRootExisted) {
            if (Test-Path -LiteralPath $svcRoot) { Remove-Item -Path $svcRoot -Recurse -Force -ErrorAction Stop }
        } else {
            foreach ($n in @('DeploySeq','DeployAck','BootFailCount','LastBootHealthy','FilterArmed','TestSigned','Signer','BootReachedUser')) {
                if ($backup.ParamProps.ContainsKey($n)) {
                    Set-ItemProperty -Path $svcRoot -Name $n -Value $backup.ParamProps[$n] -Type DWord
                } else {
                    Remove-ItemProperty -Path $svcRoot -Name $n -ErrorAction SilentlyContinue
                }
            }
        }
    } catch { $errors += "Parameters: $($_.Exception.Message)" }
    # ④ Features\Flags：键不存在→整键删 / Flags 部署前不存在→删属性 / 存在→恢复
    try {
        if (-not $backup.FeatKeyExisted) {
            if (Test-Path -LiteralPath $featKey) { Remove-Item -Path $featKey -Recurse -Force -ErrorAction Stop }
        } elseif (-not $backup.FeatExisted) {
            Remove-ItemProperty -Path $featKey -Name Flags -ErrorAction SilentlyContinue
        } else {
            Set-ItemProperty -Path $featKey -Name Flags -Value $backup.FeatFlags -Type DWord
        }
    } catch { $errors += "Features\Flags: $($_.Exception.Message)" }
    # ⑤ 计划任务：存在→Register -Xml 还原 / 不存在→Unregister
    foreach ($tn in @('RAMBoostBootGuard','RAMBoostLogonDisarm')) {
        try {
            if ($backup.TaskExisted[$tn]) {
                if (-not $backup.TaskDef[$tn]) { throw "no exported XML for pre-existing task $tn" }
                Register-ScheduledTask -TaskName $tn -Xml $backup.TaskDef[$tn] -Force -ErrorAction Stop | Out-Null
            } else {
                Unregister-ScheduledTask -TaskName $tn -Confirm:$false -ErrorAction SilentlyContinue
            }
        } catch { $errors += "task $tn`: $($_.Exception.Message)" }
    }
    # ⑥ 文件：sys32 还原或删除 + 守卫脚本还原或删除 + pdDir（部署新建则整删）
    try {
        if ($backup.Sys32Existed) {
            if ($backup.Sys32Replaced -and $backup.Sys32Backup -and (Test-Path -LiteralPath $backup.Sys32Backup)) {
                Copy-Item -LiteralPath $backup.Sys32Backup -Destination $dest -Force -ErrorAction Stop
            }
        } elseif (Test-Path -LiteralPath $dest) {
            Remove-Item -LiteralPath $dest -Force -ErrorAction Stop
        }
    } catch { $errors += "System32 sys: $($_.Exception.Message)" }
    foreach ($g in @('boot_guard.ps1','logon_disarm.ps1')) {
        $gp = Join-Path $pdDir $g
        try {
            if ($backup.GuardExisted[$g]) {
                if ($backup.GuardBackup[$g] -and (Test-Path -LiteralPath $backup.GuardBackup[$g])) {
                    Copy-Item -LiteralPath $backup.GuardBackup[$g] -Destination $gp -Force -ErrorAction Stop
                }
            } elseif (Test-Path -LiteralPath $gp) {
                Remove-Item -LiteralPath $gp -Force -ErrorAction Stop
            }
        } catch { $errors += "guard $g`: $($_.Exception.Message)" }
    }
    try {
        if (-not $backup.PdDirExisted -and (Test-Path -LiteralPath $pdDir)) {
            Remove-Item -Path $pdDir -Recurse -Force -ErrorAction Stop
        }
    } catch { $errors += "pdDir: $($_.Exception.Message)" }
    # ⑦ PFRO：恢复或删除（现状已含取消 MoveFileEx 排队替换）+ [FIX S-01] 回滚后一致性断言
    #    （既验"排队已取消"，也防误删第三方条目）
    try {
        if ($backup.PfroExisted) {
            Set-ItemProperty -Path $smKey -Name PendingFileRenameOperations -Value $backup.Pfro -Type MultiString
        } else {
            Remove-ItemProperty -Path $smKey -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
        }
    } catch { $errors += "PFRO: $($_.Exception.Message)" }
    try {
        $nowPfro = @((Get-ItemProperty -Path $smKey -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations)
        $expectPfro = if ($backup.PfroExisted) { @($backup.Pfro) } else { @() }
        if (($nowPfro -join '|') -ne ($expectPfro -join '|')) {
            $errors += ("PFRO assertion: post-rollback value differs from backup (now=[{0}] expected=[{1}])" -f ($nowPfro -join ', '), ($expectPfro -join ', '))
        }
    } catch { $errors += "PFRO assertion: $($_.Exception.Message)" }

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
    # [FIX S-01] 覆盖前一刻内容备份（紧贴覆盖点：变异可能中途失败，
    # 备份段只记存在性布尔值）。A3：备份留在 $env:TEMP，回滚不删。
    if ($backup.Sys32Existed) {
        $backup.Sys32Backup = Join-Path $env:TEMP 'RAMBlockCache.sys.predeploy'
        try {
            Copy-Item -LiteralPath $dest -Destination $backup.Sys32Backup -Force -ErrorAction Stop
        } catch {
            throw "cannot back up existing $dest before overwrite: $($_.Exception.Message)"
        }
    }
    try {
        Copy-Item -LiteralPath $srcSys -Destination $dest -Force -ErrorAction Stop
        $backup.Sys32Replaced = $true
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
    # [147/N-146-7] 写 seq 的一方必须同时让 ack 失配，否则宽限静默失效。
    # 驱动判据（rbdriver.c:281）是 depSeq != depAck 才宽限；本脚本只自增 DeploySeq、
    # 从不碰 DeployAck，而 logon_disarm 每轮登录把 ack 置成 seq。于是第二轮部署时
    # 遗留 ack 恰好等于新 seq → 不宽限 → BootFailCount+1 → 自断环 → 所有卷不挂栈。
    # 实测指纹：DeploySeq=44 DeployAck=44 BootFailCount=1 FilterArmed=0 InstanceCount=0。
    Set-ItemProperty -Path $svcRoot -Name DeployAck       -Value 0 -Type DWord
    Set-ItemProperty -Path $svcRoot -Name BootFailCount   -Value 0 -Type DWord
    Set-ItemProperty -Path $svcRoot -Name LastBootHealthy -Value 0 -Type DWord
    Set-ItemProperty -Path $svcRoot -Name FilterArmed     -Value 1 -Type DWord
    # [TICKET 2026-11-16] drop any ticket left by the previous boot: the first boot after this
    # deploy must deterministically take the DeploySeq/DeployAck grace path in RbBootFailLoad.
    Set-ItemProperty -Path $svcRoot -Name BootReachedUser -Value 0 -Type DWord
    # --- 3a. [B-6] 签名态盖章：驱动 Load 时读 TestSigned/Signer 填 GET_VERSION
    # （前端 DriverSignatureState 契约；取不到=保守 1/空=反作弊警示照弹）。
    # 判定口径：证书自签（Subject==Issuer）= 测试签名 → 1；CA 链签发 → 0。 ---
    $b6Test = 1
    try {
        $b6cert = $auth.SignerCertificate
        if ($b6cert -and "$($b6cert.Subject)" -ne "$($b6cert.Issuer)") { $b6Test = 0 }
    } catch { $b6Test = 1 }
    $b6Signer = ''
    try { if ($auth.SignerInfo -and $auth.SignerInfo.Subject) { $b6Signer = [string]$auth.SignerInfo.Subject } } catch { }
    Set-ItemProperty -Path $svcRoot -Name TestSigned -Value $b6Test -Type DWord
    if ($b6Signer) { Set-ItemProperty -Path $svcRoot -Name Signer -Value $b6Signer -Type String }
    Write-Host ('[ok] signature state stamped: TestSigned={0}, Signer={1}' -f $b6Test, $(if ($b6Signer) { $b6Signer } else { '(empty)' }))
    Write-Host ('[ok] armed: DeploySeq={0}, DeployAck=0 (grace armed), BootFailCount=0, LastBootHealthy=0, FilterArmed=1' -f ($seq + 1))

    # --- 3b. Features\Flags default = 0 ---
    if (-not (Test-Path -LiteralPath $featKey)) { New-Item -Path $featKey -Force | Out-Null }
    Set-ItemProperty -Path $featKey -Name Flags -Value 0 -Type DWord
    Write-Host '[ok] Features\Flags=0 (all feature flags off)'

    # --- 4. Install guard scripts ---
    if (-not (Test-Path -LiteralPath $pdDir)) { New-Item -ItemType Directory -Path $pdDir -Force | Out-Null }
    # [FIX S-01] 覆盖前一刻内容备份（紧贴覆盖点，同 System32 备份原则）。
    foreach ($g in @('boot_guard.ps1','logon_disarm.ps1')) {
        if ($backup.GuardExisted[$g]) {
            $gb = Join-Path $env:TEMP "RAMBoost.$g.predeploy"
            try {
                Copy-Item -LiteralPath (Join-Path $pdDir $g) -Destination $gb -Force -ErrorAction Stop
                $backup.GuardBackup[$g] = $gb
            } catch {
                throw "cannot back up existing guard script ${g}: $($_.Exception.Message)"
            }
        }
    }
    Copy-Item (Join-Path $scriptsDir 'boot_guard.ps1')   (Join-Path $pdDir 'boot_guard.ps1')   -Force -ErrorAction Stop
    Copy-Item (Join-Path $scriptsDir 'logon_disarm.ps1') (Join-Path $pdDir 'logon_disarm.ps1') -Force -ErrorAction Stop
    Write-Host "[ok] guard scripts installed to $pdDir"

    # --- 5. Register scheduled tasks ---
    $bgPath = Join-Path $pdDir 'boot_guard.ps1'
    $ldPath = Join-Path $pdDir 'logon_disarm.ps1'

    # [147/N-146-8] Register-ScheduledTask 的返回对象与 -Force 的"成功"都不可信：
    # 本轮实测到"打印 [ok] registered 但任务动作仍是旧快照"。判定必须读
    # C:\Windows\System32\Tasks\<name> 的真实磁盘 XML，并核对动作里引用的脚本。
    # 任务 XML 为 UTF-16LE（带 FF FE BOM），不能假定 Get-Content 的默认编码。
    $AssertTaskOnDisk = {
        param([string]$Name, [string]$MustReference)
        $disk = Join-Path $env:SystemRoot "System32\Tasks\$Name"
        if (-not (Test-Path -LiteralPath $disk)) {
            throw "task ${Name}: Register-ScheduledTask returned success but no on-disk XML at $disk"
        }
        $bytes = [System.IO.File]::ReadAllBytes($disk)
        $u = [System.Text.Encoding]::Unicode.GetString($bytes)
        $a = [System.Text.Encoding]::UTF8.GetString($bytes)
        $hit = ($u.IndexOf($MustReference, [System.StringComparison]::Ordinal) -ge 0) -or
               ($a.IndexOf($MustReference, [System.StringComparison]::Ordinal) -ge 0)
        if (-not $hit) {
            # 失败代价不对称：这里抛异常会触发 deploy 回滚，把一次**成功**的部署
            # 撤掉（还原 UpperFilters/服务配置）。因此内容匹配只作 WARN 取证，
            # 存在性才是硬门（"报 ok 但其实没建"才是 N-146-8 的本体）。
            Write-Host ("  [!!] task {0}: on-disk XML does not contain '{1}' -- inspect {2}" -f $Name, $MustReference, $disk) -ForegroundColor Yellow
        } else {
            Write-Host ('[ok] task {0} verified on disk ({1} B, references {2})' -f $Name, $bytes.Length, $MustReference)
        }
    }

    $act1 = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$bgPath`""
    $trg1 = New-ScheduledTaskTrigger -AtStartup
    $prin = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName 'RAMBoostBootGuard' -Action $act1 -Trigger $trg1 -Principal $prin -Force | Out-Null
    & $AssertTaskOnDisk 'RAMBoostBootGuard' 'boot_guard.ps1'
    Write-Host '[ok] scheduled task RAMBoostBootGuard registered (SYSTEM, AtStartup)'

    $act2 = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$ldPath`""
    $trg2 = New-ScheduledTaskTrigger -AtLogOn
    Register-ScheduledTask -TaskName 'RAMBoostLogonDisarm' -Action $act2 -Trigger $trg2 -Principal $prin -Force | Out-Null
    & $AssertTaskOnDisk 'RAMBoostLogonDisarm' 'logon_disarm.ps1'
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
