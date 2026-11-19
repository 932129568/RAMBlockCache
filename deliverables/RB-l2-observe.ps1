#requires -Version 5.1
<#
.SYNOPSIS
    RAMBlockCache L2 只读观测（V1~V6 六项检查，只出证据，不修任何东西）。

.DESCRIPTION
    绝对只读：不写注册表、不写卷、不调 rbfeatctl enable/disable、不碰 build\。
    每项固定一行：[PASS] Vn ... / [FAIL] Vn ... / [UNKNOWN] Vn ...

    V1 镜像      System32\drivers\RAMBlockCache.sys，分层判据（不再"不等于 pin 就 UNKNOWN"）：
                 ① 三锚点（deploy.ps1 / verify_deploy_signature.ps1 的 DefaultExpectedSha256、
                    dist\RAMBlockCache.sys.sha256）必须先自洽，互相矛盾 = FAIL；
                 ② 锚点自洽且部署件哈希 == 锚点 → 再验 Authenticode，Valid 才 PASS，否则 FAIL；
                 ③ 部署件 == expected-sys-sha.txt 且锚点不可用 → PASS（未签名镜像的旧语义）；
                 ④ 部署件既不等于锚点也不等于 pin → FAIL（可疑换包，三个值全打印）；
                 ⑤ 锚点与 pin 都确立不了 → UNKNOWN（fail-open 到 UNKNOWN，不造 PASS）。
    V2 服务      Get-Service RAMBlockCache 是否 Running。
    V3 崩溃      boot 之后 System 日志 Id=1001(BugCheck)/Id=41(Kernel-Power) 条数；
                 Minidump / MEMORY.DMP 的 mtime 是否晚于 boot（晚于 = FAIL）。
    V4 引导日志  driver-<yyyyMMdd>.log 里 boot 之后的行：总行数 / ERROR / l2:，
                 close deferred / close aborted / self-disable / SafeMode 命中单列。
                 今日文件缺失时回退到目录内最新的 driver-*.log（跨零点场景）。
                 无日志/无本引导段 = UNKNOWN：日志是"证据"而非被测对象，缺失既可能是
                 驱动未写（V2 已独立覆盖）也可能是日志开关/权限/跨日，无法唯一归因；
                 审计脚本不能把"缺证据"当 PASS，也不该把配置性缺失判 FAIL。
    V5 Verifier  verifier.exe /query 里本驱动的 加载/卸载/特殊池/失败分配；
                 需提权或读取失败则 UNKNOWN（不绕过）；任一项解析失败也 UNKNOWN，
                 绝不把"没解析到"当成 0。
    V6 桥接      http://127.0.0.1:5199/health 的 ok / native / deviceOpen
                 （8787 是系统代理回的假 502，别用）。三者必须同时为真才算 PASS；
                 布尔值按严格比较（字符串 "false" 不得被 [bool] 转成 True）。

    退出码：全 PASS=0；有 FAIL=1；无 FAIL 但有 UNKNOWN=2。

.PARAMETER LogDir
    驱动日志目录，默认 C:\ProgramData\RAMBlockCache\logs。

.PARAMETER PinFile
    sha256 pin 文件，默认脚本同目录的 expected-sys-sha.txt。

.PARAMETER BridgePort
    桥接端口，默认 5199。

.PARAMETER DumpDir
    转储目录，默认 %SystemRoot%\Minidump。仅用于把 V3 指向临时目录做故障注入，
    默认行为与原脚本完全一致（不碰系统目录内容，只读取）。

.PARAMETER VerifierTextFile
    仅用于故障注入：给出后用该文件内容替代 `verifier /query` 的输出参与 V5 解析，
    便于在英文/异常输出下验证解析路径。默认空 = 真实调用 verifier。

.PARAMETER AnchorRoot
    仅用于故障注入：V1 三锚点（src\scripts\deploy.ps1、
    src\scripts\verify_deploy_signature.ps1、dist\RAMBlockCache.sys.sha256）的读取根目录。
    默认空 = 从脚本所在目录逐级向上自动定位包含 src\scripts\deploy.ps1 的仓库根。
    给一个锚点**副本**目录即可在不触碰 src\ 的前提下验证 V1 的 FAIL 路径。

.EXAMPLE
    pwsh -NoProfile -File deliverables\RB-l2-observe.ps1

.EXAMPLE
    powershell -NoProfile -File deliverables\RB-l2-observe.ps1
#>
[CmdletBinding()]
param(
    [string]$LogDir = 'C:\ProgramData\RAMBlockCache\logs',
    [string]$PinFile = '',
    [int]$BridgePort = 5199,
    [string]$DumpDir = '',
    [string]$VerifierTextFile = '',
    [string]$AnchorRoot = ''
)

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$script:Results = New-Object System.Collections.ArrayList

function Emit {
    param([string]$Level, [string]$Id, [string]$Text)
    Write-Host ('[' + $Level + '] ' + $Id + ' ' + $Text)
    [void]$script:Results.Add([pscustomobject]@{ Level = $Level; Id = $Id })
}

function EmitPass { param([string]$Id, [string]$Text) Emit 'PASS' $Id $Text }
function EmitFail { param([string]$Id, [string]$Text) Emit 'FAIL' $Id $Text }
function EmitUnknown { param([string]$Id, [string]$Text) Emit 'UNKNOWN' $Id $Text }

function Format-Time {
    param($Value)
    if ($null -eq $Value) { return '(未知)' }
    return ([datetime]$Value).ToString('yyyy-MM-dd HH:mm:ss')
}

# 显式计数：绝不依赖 $null 的 .Count（本仓库有过 $null 被算成 1 元素 → 假 PASS 的事故）
function Get-Count {
    param($Value)
    if ($null -eq $Value) { return 0 }
    return @($Value).Count
}

# 驱动仍在写日志：用 ReadWrite 共享打开，只读不锁，避免日志被独占时读不到
function Read-SharedText {
    param([string]$Path)
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $sr = New-Object System.IO.StreamReader($fs)
        try { return $sr.ReadToEnd() } finally { $sr.Dispose() }
    } finally {
        $fs.Dispose()
    }
}

$script:StartTicks = (Get-Date).Ticks

Write-Host '=== RAMBlockCache L2 只读观测 ==='
Write-Host ('时间 ' + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') + '   主机 ' + $env:COMPUTERNAME + '   PS ' + $PSVersionTable.PSVersion.ToString())
Write-Host ''

# -----------------------------------------------------------------------------
# boot 时间（V3/V4 都要用；取不到就让这两项走 UNKNOWN，绝不猜）
# -----------------------------------------------------------------------------
$boot = $null
try {
    $os = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop
    if ($null -ne $os -and $null -ne $os.LastBootUpTime) { $boot = [datetime]$os.LastBootUpTime }
} catch {
    $boot = $null
}

# -----------------------------------------------------------------------------
# V1 镜像哈希 —— 分层判据
#
# [改] 旧逻辑只有一条"部署件哈希 == expected-sys-sha.txt 才 PASS，不等就 UNKNOWN"。
# pin 是**签名前**产物哈希，已签名部署态下它永远不等 —— V1 恒为 UNKNOWN，等于废掉：
# 既证明不了镜像对，也无法区分"哈希变了但签名链是对的"和"被人换成了野包"。
# 现改为分层（顺序即优先级）：
#   ① 三锚点先自洽（deploy.ps1 / verify_deploy_signature.ps1 的 DefaultExpectedSha256
#      与 dist 边车）；互相矛盾 = FAIL（只改其中一处本身就是事故）。
#   ② 锚点自洽且部署件 == 锚点 → 再过 Authenticode：Valid 才 PASS，非 Valid = FAIL。
#   ③ 部署件 == pin 且锚点不可确立 → PASS（未签名镜像的老语义保留）。
#   ④ 部署件既不等于锚点也不等于 pin → FAIL（可疑换包），三个值全打印。
#   ⑤ 锚点与 pin 都确立不了 → UNKNOWN（保持 fail-open 到 UNKNOWN 的纪律，不造 PASS）。
#
# 哈希一律用 [0-9A-Fa-f]{64} 现取，不写死行号/列位置：三个文件里
# DefaultExpectedSha256 的写法（单引号 / 双引号 / 裸值 / 非字面量）与边车文件写法
# （"<hash>"、"<hash>  <文件名>"、"SHA256: <hash>"）都能覆盖。
# -----------------------------------------------------------------------------
$sysPath = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'

$hereDir = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($hereDir)) {
    try { $hereDir = Split-Path -Parent $MyInvocation.MyCommand.Path } catch { $hereDir = '' }
}
if ([string]::IsNullOrWhiteSpace($hereDir)) { $hereDir = (Get-Location).Path }

# 取文本里第一个独立成词的 64 位十六进制串（前后不得紧邻十六进制字符）
function Get-FirstHash64 {
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $m = [regex]::Match($Text, '(?<![0-9A-Fa-f])([0-9A-Fa-f]{64})(?![0-9A-Fa-f])')
    if ($m.Success) { return $m.Groups[1].Value.ToLowerInvariant() }
    return $null
}

# DefaultExpectedSha256 锚点：先认赋值行内的字面量；赋值是变量时退化为文件内首个 64-hex
function Get-AnchorFromPs1 {
    param([string]$Path)
    $res = [pscustomobject]@{ Hash = $null; How = '' }
    if ([string]::IsNullOrWhiteSpace($Path)) { $res.How = '路径为空'; return $res }
    if (-not (Test-Path -LiteralPath $Path)) { $res.How = '文件不存在'; return $res }
    try {
        $text = [System.IO.File]::ReadAllText($Path)
    } catch {
        $res.How = '文件不可读'
        return $res
    }
    $m = [regex]::Match($text, 'DefaultExpectedSha256\s*=\s*[''"]?(?<h>[0-9A-Fa-f]{64})[''"]?')
    if ($m.Success) {
        $res.Hash = $m.Groups['h'].Value.ToLowerInvariant()
        $res.How = 'DefaultExpectedSha256 字面量'
        return $res
    }
    $h = Get-FirstHash64 $text
    if ($null -ne $h) {
        $res.Hash = $h
        $res.How = 'DefaultExpectedSha256 非字面量，退化为文件内首个 64-hex'
    } else {
        $res.How = '无 64-hex 哈希'
    }
    return $res
}

# 边车锚点：逐行找第一个非空非注释行里的哈希；兼容裸哈希 / sha256sum / "SHA256: <hash>"
function Get-AnchorFromSidecar {
    param([string]$Path)
    $res = [pscustomobject]@{ Hash = $null; How = '' }
    if ([string]::IsNullOrWhiteSpace($Path)) { $res.How = '路径为空'; return $res }
    if (-not (Test-Path -LiteralPath $Path)) { $res.How = '文件不存在'; return $res }
    try {
        $lines = @(Get-Content -LiteralPath $Path -ErrorAction Stop)
    } catch {
        $res.How = '文件不可读'
        return $res
    }
    foreach ($ln in $lines) {
        if ($null -eq $ln) { continue }
        $t = ([string]$ln).Trim()
        if ($t.Length -eq 0) { continue }
        if ($t.StartsWith('#')) { continue }
        $h = Get-FirstHash64 $t
        if ($null -ne $h) {
            $res.Hash = $h
            $res.How = '边车首行哈希'
            return $res
        }
    }
    $res.How = '无 64-hex 哈希行'
    return $res
}

# 锚点根：默认从脚本目录逐级上溯，找到含 src\scripts\deploy.ps1 的那一层（最多 4 层）
$anchorRoot = $AnchorRoot
if ([string]::IsNullOrWhiteSpace($anchorRoot)) {
    $probe = $hereDir
    for ($i = 0; $i -lt 4; $i++) {
        if ([string]::IsNullOrWhiteSpace($probe)) { break }
        if ((Test-Path -LiteralPath (Join-Path $probe 'src\scripts\deploy.ps1')) -or
            (Test-Path -LiteralPath (Join-Path $probe 'dist\RAMBlockCache.sys.sha256'))) {
            $anchorRoot = $probe
            break
        }
        $up = Split-Path -Parent $probe
        if ([string]::IsNullOrWhiteSpace($up) -or $up -eq $probe) { break }
        $probe = $up
    }
}
if ([string]::IsNullOrWhiteSpace($anchorRoot)) { $anchorRoot = $hereDir }

$aDeploy  = Get-AnchorFromPs1     (Join-Path $anchorRoot 'src\scripts\deploy.ps1')
$aVerify  = Get-AnchorFromPs1     (Join-Path $anchorRoot 'src\scripts\verify_deploy_signature.ps1')
$aSidecar = Get-AnchorFromSidecar (Join-Path $anchorRoot 'dist\RAMBlockCache.sys.sha256')

$anchorNames = @('deploy.ps1', 'verify_deploy_signature.ps1', 'dist-sidecar')
$anchorObjs = @($aDeploy, $aVerify, $aSidecar)
$anchorRead = 0
$anchorDistinct = New-Object System.Collections.ArrayList
$anchorText = ''
for ($ai = 0; $ai -lt 3; $ai++) {
    $ah = $anchorObjs[$ai].Hash
    if ($null -ne $ah) {
        $anchorRead = $anchorRead + 1
        if (-not $anchorDistinct.Contains($ah)) { [void]$anchorDistinct.Add($ah) }
        $anchorText = $anchorText + $anchorNames[$ai] + '=' + $ah + '；'
    } else {
        $anchorText = $anchorText + $anchorNames[$ai] + '=(不可用:' + $anchorObjs[$ai].How + ')；'
    }
}

# 至少 2 个锚点可读且取值唯一，才算"锚点自洽"；多于一个取值即"锚点矛盾"
$anchorConflict = ($anchorRead -ge 2 -and $anchorDistinct.Count -gt 1)
$anchorUsable = ($anchorRead -ge 2 -and $anchorDistinct.Count -eq 1)
$anchorHash = $null
if ($anchorUsable) { $anchorHash = [string]$anchorDistinct[0] }
$anchorEvidence = '锚点(read=' + $anchorRead + '/3)：' + $anchorText + '根=' + $anchorRoot

$pinPath = $PinFile
if ([string]::IsNullOrWhiteSpace($pinPath)) {
    $pinPath = Join-Path $hereDir 'expected-sys-sha.txt'
}

$pinHash = $null
$pinEvidence = ''
try {
    if (Test-Path -LiteralPath $pinPath) {
        $pinLines = @(Get-Content -LiteralPath $pinPath -ErrorAction Stop)
        foreach ($pl in $pinLines) {
            if ($null -eq $pl) { continue }
            $pinLine = ([string]$pl).Trim()
            if ($pinLine.Length -eq 0) { continue }        # 跳过空行/纯空格行
            if ($pinLine.StartsWith('#')) { continue }     # 跳过注释行
            # 同时接受裸哈希与 sha256sum 的 "<hash>  <文件名>" 两种写法
            $hm = [regex]::Match($pinLine, '(?<![0-9A-Fa-f])([0-9A-Fa-f]{64})(?![0-9A-Fa-f])')
            if ($hm.Success) {
                $pinHash = $hm.Groups[1].Value.ToLowerInvariant()
                $pinEvidence = $pinHash
                break
            }
        }
        if ($null -eq $pinHash) {
            $pinEvidence = '文件内无 64 位十六进制哈希行'
        }
    } else {
        $pinEvidence = '文件不存在'
    }
} catch {
    $pinHash = $null
    $pinEvidence = '文件不可读'
}

$sysHash = $null
try {
    if (Test-Path -LiteralPath $sysPath) {
        $sysHash = (Get-FileHash -LiteralPath $sysPath -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
    }
} catch {
    $sysHash = $null
}

$sigState = ''
$sigSigner = ''
$pinText = 'pin=' + $pinEvidence

if ($null -eq $sysHash) {
    EmitFail 'V1' ('镜像不存在或哈希不可读：' + $sysPath)
} elseif ($anchorConflict) {
    # ① 锚点自身互相矛盾：先修锚点，任何"哈希对得上"的结论都无意义
    EmitFail 'V1' ('三锚点互相矛盾，可信基线不成立（read=' + $anchorRead + '/3）：' + $anchorText + '根=' + $anchorRoot)
} elseif ($anchorUsable -and $sysHash -eq $anchorHash) {
    # ② 哈希与自洽锚点一致 → 必须再过 Authenticode 才算 PASS
    try {
        $sig = Get-AuthenticodeSignature -LiteralPath $sysPath -ErrorAction Stop
    } catch {
        $sig = $null
    }
    if ($null -eq $sig) {
        $sigState = '(签名对象取不到)'
    } else {
        $sigState = [string]$sig.Status
        if ($null -ne $sig.SignerCertificate) { $sigSigner = ' signer=' + [string]$sig.SignerCertificate.Subject }
    }
    if ($sigState -eq 'Valid') {
        EmitPass 'V1' ('部署件与三锚点一致且签名有效：sha256=' + $sysHash + '；' + $anchorEvidence + '；Authenticode=Valid' + $sigSigner)
    } else {
        EmitFail 'V1' ('哈希与三锚点一致但 Authenticode 非 Valid（=' + $sigState + '）' + $sigSigner + '；sha256=' + $sysHash + '；' + $anchorEvidence)
    }
} elseif ($anchorUsable) {
    # 锚点自洽但部署件不等于锚点：命中 pin 属于"把未签名产物部署到已签名基线上"，同样是缺陷
    if ($null -ne $pinHash -and $sysHash -eq $pinHash) {
        EmitFail 'V1' ('部署件等于未签名 pin，但三锚点要求已签名构建（降级部署）：实际=' + $sysHash + '；' + $anchorEvidence + '；' + $pinText)
    } else {
        # ④ 既不是锚点值也不是 pin 值 → 可疑换包，三个值全打印
        EmitFail 'V1' ('可疑换包：部署件哈希既不是三锚点值也不是 pin 值。实际=' + $sysHash +
                       '；锚点值=' + $anchorHash + '；pin值=' + $(if ($null -ne $pinHash) { $pinHash } else { '(不可用)' }) +
                       '；' + $anchorEvidence + '；' + $pinText)
    }
} elseif ($null -ne $pinHash -and $sysHash -eq $pinHash) {
    # ③ 锚点不可确立但命中 pin：保留"未签名镜像"的 PASS 语义
    EmitPass 'V1' ('sha256=' + $sysHash + ' 与 pin 一致（pin=' + $pinEvidence + '；' + $anchorEvidence + '）')
} elseif ($null -eq $pinHash) {
    # ⑤ 锚点与 pin 全不可用：不知道基线是什么，只能 UNKNOWN
    EmitUnknown 'V1' ('实际 sha256=' + $sysHash + '；锚点与 pin 均不可用，无法判定：' + $anchorEvidence + '；pin 不可用（' + $pinEvidence + '）：' + $pinPath)
} else {
    # 锚点不可确立、pin 可读但不匹配：没有可信基线，不能升级为 FAIL（可能只是锚点缺失）
    EmitUnknown 'V1' ('实际 sha256=' + $sysHash + ' 与 pin 不一致且锚点不可确立（' + $anchorEvidence + '）；' + $pinText)
}

# -----------------------------------------------------------------------------
# V2 服务
# -----------------------------------------------------------------------------
$svc = $null
try {
    $svc = Get-Service -Name 'RAMBlockCache' -ErrorAction Stop
} catch {
    $svc = $null
}

if ($null -eq $svc) {
    EmitFail 'V2' 'Get-Service RAMBlockCache 取不到（服务未注册？）'
} elseif ([string]$svc.Status -eq 'Running') {
    EmitPass 'V2' ('RAMBlockCache = ' + [string]$svc.Status)
} else {
    EmitFail 'V2' ('RAMBlockCache = ' + [string]$svc.Status + '（期望 Running）')
}

# -----------------------------------------------------------------------------
# V3 崩溃证据
# -----------------------------------------------------------------------------
if ($null -eq $boot) {
    EmitUnknown 'V3' '取不到 LastBootUpTime，无法切分本引导'
} else {
    $bugCheckCount = -2
    $powerCount = -2

    foreach ($evId in @(1001, 41)) {
        $n = -2
        try {
            $raw = Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = $evId; StartTime = $boot } -ErrorAction Stop
            $n = Get-Count $raw
        } catch {
            $ex = $_.Exception
            $msg = ''
            if ($null -ne $ex) { $msg = [string]$ex.Message }
            $typeName = ''
            if ($null -ne $ex) { $typeName = [string]$ex.GetType().FullName }
            # 无匹配事件时 Get-WinEvent 会抛 EventLogNotFoundException / 本地化消息
            if ($typeName -match 'EventLogNotFoundException' -or
                $msg -match 'No events were found|No matching events|未找到|找不到|没有找到') {
                $n = 0
            } else {
                $n = -1
            }
        }
        if ($evId -eq 1001) { $bugCheckCount = $n } else { $powerCount = $n }
    }

    # 转储文件：mtime 晚于 boot 即视为本次引导新增
    $dumpNewCount = 0
    $dumpTotal = 0
    $dumpState = ''
    $newestDumpTime = $null
    $dumpDir = $DumpDir
    if ([string]::IsNullOrWhiteSpace($dumpDir)) { $dumpDir = Join-Path $env:SystemRoot 'Minidump' }
    try {
        if (Test-Path -LiteralPath $dumpDir) {
            # 只认 *.dmp：目录里混入的 README/日志等非转储文件不得造成假 FAIL
            $dumps = Get-ChildItem -LiteralPath $dumpDir -File -Filter '*.dmp' -ErrorAction Stop
            $dumpTotal = Get-Count $dumps
            if ($null -ne $dumps) {
                foreach ($d in @($dumps)) {
                    # 与 $boot 同源同区：两者都是本地墙钟时间（Kind=Local），比较不跨时区
                    if ($null -eq $newestDumpTime -or [datetime]$d.LastWriteTime -gt $newestDumpTime) {
                        $newestDumpTime = [datetime]$d.LastWriteTime
                    }
                    if ([datetime]$d.LastWriteTime -gt $boot) { $dumpNewCount = $dumpNewCount + 1 }
                }
            }
            $dumpState = '目录存在'
        } else {
            $dumpState = '目录不存在'
        }
    } catch {
        $dumpNewCount = -1
        $dumpTotal = -1
        $dumpState = '不可读'
    }

    $memDmpPath = Join-Path $env:SystemRoot 'MEMORY.DMP'
    $memDmpNew = $false
    $memDmpState = '不存在'
    try {
        if (Test-Path -LiteralPath $memDmpPath) {
            $mi = Get-Item -LiteralPath $memDmpPath -ErrorAction Stop
            if ($null -ne $mi) {
                if ([datetime]$mi.LastWriteTime -gt $boot) { $memDmpNew = $true }
                $memDmpState = 'mtime=' + ([datetime]$mi.LastWriteTime).ToString('yyyy-MM-dd HH:mm:ss') + '(' + ([datetime]$mi.LastWriteTime).Kind + ')'
            } else {
                $memDmpState = '不可读'
            }
        }
    } catch {
        $memDmpState = '不可读'
    }

    $dumpTimeText = '(无)'
    if ($null -ne $newestDumpTime) {
        # 打印被比较的第二个值本身，并标注 Kind，证明与 boot 同为本地时间
        $dumpTimeText = (Format-Time $newestDumpTime) + '(' + $newestDumpTime.Kind + ')'
    }

    $detail = 'boot=' + (Format-Time $boot) + '(' + $boot.Kind + ')' +
              '；BugCheck(1001)=' + $bugCheckCount +
              '；KernelPower(41)=' + $powerCount +
              '；Minidump 新于 boot=' + $dumpNewCount + '/' + $dumpTotal + '（' + $dumpState + '：' + $dumpDir + '）' +
              '；Minidump 最新 mtime=' + $dumpTimeText +
              '；MEMORY.DMP 新于 boot=' + $(if ($memDmpNew) { '是' } else { '否' }) + '（' + $memDmpState + '）'

    if ($bugCheckCount -lt 0 -or $powerCount -lt 0 -or $dumpNewCount -lt 0) {
        EmitUnknown 'V3' ('System 日志/转储目录读取失败（可能需提权），' + $detail)
    } elseif ($bugCheckCount -gt 0 -or $powerCount -gt 0 -or $dumpNewCount -gt 0 -or $memDmpNew) {
        EmitFail 'V3' $detail
    } else {
        EmitPass 'V3' $detail
    }
}

# -----------------------------------------------------------------------------
# V4 本次引导段驱动日志
# -----------------------------------------------------------------------------
$todayStamp = (Get-Date).ToString('yyyyMMdd')
$todayLog = Join-Path $LogDir ('driver-' + $todayStamp + '.log')

$logPath = $todayLog
$logText = $null
$logNote = ''
try {
    if (Test-Path -LiteralPath $todayLog) {
        $logPath = $todayLog
        $logText = Read-SharedText $todayLog
    } else {
        # 跨零点：今日尚无写入（boot 在昨天）时回退到目录内最新的 driver-*.log，
        # 否则会把"日志在昨天的文件里"误判成"没有日志"。
        $hist = @()
        try {
            if (Test-Path -LiteralPath $LogDir) {
                $hist = @(Get-ChildItem -LiteralPath $LogDir -File -Filter 'driver-*.log' -ErrorAction Stop | Sort-Object Name -Descending)
            }
        } catch {
            $hist = @()
        }
        if ($hist.Count -gt 0) {
            $logPath = $hist[0].FullName
            $logText = Read-SharedText $logPath
            $logNote = '今日文件不存在，回退到最新日志'
        } else {
            $logNote = '今日文件不存在且目录内无历史日志'
        }
    }
} catch {
    $logText = $null
    $logNote = '读取异常'
}

if ($null -eq $logText) {
    EmitUnknown 'V4' ('驱动日志不可读：' + $logPath + '（' + $logNote + '）')
} elseif ($null -eq $boot) {
    EmitUnknown 'V4' '取不到 LastBootUpTime，无法切出本引导段'
} else {
    $rawLines = $logText -split "`n"
    $inSegment = $false
    $total = 0
    $errCount = 0
    $l2Count = 0
    $segFirst = $null
    $segLast = $null
    $lastLocal = $null
    $flagHits = New-Object System.Collections.ArrayList

    $keywords = @('close deferred', 'close aborted', 'self-disable', 'SafeMode')

    foreach ($raw in $rawLines) {
        $line = $raw.TrimEnd("`r")
        if ($line.Length -eq 0) { continue }

        # 行首 UTC 时间戳：2026-11-12T03:14:26.359Z
        $m = [regex]::Match($line, '^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?)Z')
        if ($m.Success) {
            try {
                $utc = [datetime]::Parse($m.Groups[1].Value,
                    [System.Globalization.CultureInfo]::InvariantCulture,
                    [System.Globalization.DateTimeStyles]::AdjustToUniversal -bor [System.Globalization.DateTimeStyles]::AssumeUniversal)
                # 日志是 UTC(Z)，boot 是本地时间：必须同一时区再比，否则恒偏 8 小时
                $local = $utc.ToLocalTime()
                $lastLocal = $local
                if ($local -ge $boot) { $inSegment = $true } else { $inSegment = $false }
            } catch {
                # 时间戳解析失败：沿用上一行的段状态，不丢行
            }
        }

        if (-not $inSegment) { continue }

        $total = $total + 1
        if ($null -eq $segFirst -and $null -ne $lastLocal) { $segFirst = $lastLocal }
        if ($null -ne $lastLocal) { $segLast = $lastLocal }
        # 大小写不敏感（-match 默认 IgnoreCase），但要求独立词边界：
        # 小写 error: 能命中，ERRORS=/no-error 这类子串不误报
        if ($line -match '(?<![0-9A-Za-z_])ERROR(?![0-9A-Za-z_])') { $errCount = $errCount + 1 }
        if ($line -match 'l2:') { $l2Count = $l2Count + 1 }
        foreach ($kw in $keywords) {
            if ($line -match [regex]::Escape($kw)) { [void]$flagHits.Add($kw + ' -> ' + $line) }
        }
    }

    $segInfo = ''
    if ($null -ne $segFirst) {
        $segInfo = '；段首=' + (Format-Time $segFirst) + ' 段尾=' + (Format-Time $segLast)
    }
    $detail = '文件=' + $logPath + $(if ($logNote.Length -gt 0) { '（' + $logNote + '）' } else { '' }) +
              '；boot=' + (Format-Time $boot) + '(' + $boot.Kind + ')' + $segInfo + '（均为本地时间）' +
              '；本引导段行数=' + $total + '；ERROR=' + $errCount + '；l2:=' + $l2Count

    foreach ($hit in $flagHits) {
        Write-Host ('       [HIT] V4 ' + $hit)
    }

    if ($total -eq 0) {
        EmitUnknown 'V4' ($detail + '（本引导段无日志行：驱动未写日志/日志开关关闭/权限不足，无法据此判定，不当 PASS）')
    } elseif ($errCount -gt 0) {
        EmitFail 'V4' ($detail + '；本引导段出现 ERROR')
    } elseif ($flagHits.Count -gt 0) {
        EmitFail 'V4' ($detail + '；风险关键字命中=' + $flagHits.Count)
    } else {
        EmitPass 'V4' ($detail + '；无 ERROR / close deferred / close aborted / self-disable / SafeMode')
    }
}

# -----------------------------------------------------------------------------
# V5 Verifier
# -----------------------------------------------------------------------------
$verifierExe = Join-Path $env:SystemRoot 'System32\verifier.exe'
$vOut = $null
$vExit = -1
$vSource = 'verifier /query'
if (-not [string]::IsNullOrWhiteSpace($VerifierTextFile)) {
    # 故障注入通道：用文件内容替代 verifier 输出（只读，不改变默认路径）
    $vSource = '注入文本 ' + $VerifierTextFile
    try {
        $injText = [System.IO.File]::ReadAllText($VerifierTextFile)
        $vOut = @($injText -split "`r?`n")
        $vExit = 0
    } catch {
        $vOut = $null
        $vExit = -1
    }
} else {
    try {
        $vOut = @(& $verifierExe /query 2>&1 | ForEach-Object { [string]$_ })
        $vExit = $LASTEXITCODE
    } catch {
        $vOut = $null
        $vExit = -1
    }
}

$vText = ''
if ($null -ne $vOut) { $vText = ($vOut -join "`n") }
$vCount = Get-Count $vOut

$blocked = $false
if ($null -eq $vOut -or $vCount -eq 0) { $blocked = $true }
if ($vText -match '拒绝访问|Access is denied|需要提升|must be run as') { $blocked = $true }
if ($vExit -ne 0 -and $vText -notmatch 'RAMBlockCache\.sys') { $blocked = $true }

if ($blocked) {
    $head = $vText
    if ($head.Length -gt 120) { $head = $head.Substring(0, 120) }
    EmitUnknown 'V5' ('verifier /query 读取失败（来源=' + $vSource + '，exit=' + $vExit + '，可能需管理员；未绕过）：' + $head)
} elseif ($vText -notmatch 'RAMBlockCache\.sys') {
    EmitUnknown 'V5' ('verifier 列表中未出现 RAMBlockCache.sys（本驱动未被验证）；来源=' + $vSource + '，exit=' + $vExit)
} else {
    # 中文/英文两套关键字分别取计数；任何一项取不到都保持 -1（未知），绝不默认 0
    $loaded = -1
    $unloaded = -1
    $ml = [regex]::Match($vText, '(?<![A-Za-z])加载[:：]\s*(\d+)')
    if (-not $ml.Success) { $ml = [regex]::Match($vText, '(?<![A-Za-z])Loaded[:：]\s*(\d+)') }
    if ($ml.Success) { $loaded = [int]$ml.Groups[1].Value }
    $mu = [regex]::Match($vText, '(?<![A-Za-z])卸载[:：]\s*(\d+)')
    if (-not $mu.Success) { $mu = [regex]::Match($vText, '(?<![A-Za-z])Unloaded[:：]\s*(\d+)') }
    if ($mu.Success) { $unloaded = [int]$mu.Groups[1].Value }

    $specialPool = '未知'
    $ms = [regex]::Match($vText, '\[\s*([Xx ])\s*\]\s*0x00000001')
    if ($ms.Success) {
        if ($ms.Groups[1].Value -match '[Xx]') { $specialPool = '开' } else { $specialPool = '关' }
    }

    # "故意失败的池分配"包含子串"失败的池分配"，必须排除，否则计数张冠李戴
    $failedAlloc = -1
    $mf = [regex]::Match($vText, '(?<!故意)失败的池分配[:：]\s*(\d+)')
    if (-not $mf.Success) { $mf = [regex]::Match($vText, '(?<!Deliberately )Failed Allocations[:：]\s*(\d+)') }
    if ($mf.Success) { $failedAlloc = [int]$mf.Groups[1].Value }

    $detail = 'RAMBlockCache.sys 加载=' + $loaded + ' 卸载=' + $unloaded +
              ' 特殊池=' + $specialPool + ' 失败分配=' + $failedAlloc + '（来源=' + $vSource + '）'

    if ($loaded -lt 0 -or $unloaded -lt 0 -or $failedAlloc -lt 0) {
        EmitUnknown 'V5' ($detail + '（计数未全部解析，不做 PASS 结论）')
    } elseif ($failedAlloc -gt 0) {
        EmitFail 'V5' $detail
    } elseif ($specialPool -ne '开') {
        EmitUnknown 'V5' ($detail + '（特殊池未开，本基线不含特殊池结论）')
    } else {
        EmitPass 'V5' $detail
    }
}

# -----------------------------------------------------------------------------
# V6 桥接健康
# -----------------------------------------------------------------------------
$healthUrl = 'http://127.0.0.1:' + $BridgePort + '/health'
$health = $null
try {
    $health = Invoke-RestMethod -Uri $healthUrl -Method Get -TimeoutSec 3 -ErrorAction Stop
} catch {
    $health = $null
}

if ($null -eq $health) {
    EmitUnknown 'V6' ('桥接不可达或响应非 JSON：' + $healthUrl + '（端口 5199，勿用 8787）')
} else {
    $okVal = $null
    $nativeVal = $null
    $devVal = $null
    # 严格布尔：JSON 里的字符串 "false" 不能被 [bool] 转成 True（PowerShell 非空字符串恒为真）
    if ($null -ne $health.ok) { $okVal = [bool]($health.ok -eq $true) }
    if ($null -ne $health.native) { $nativeVal = [bool]($health.native -eq $true) }
    if ($null -ne $health.deviceOpen) { $devVal = [bool]($health.deviceOpen -eq $true) }

    $errText = ''
    if ($null -ne $health.pluginError -and [string]$health.pluginError -ne '') { $errText = $errText + ' pluginError=' + [string]$health.pluginError }
    if ($null -ne $health.deviceError -and [string]$health.deviceError -ne '') { $errText = $errText + ' deviceError=' + [string]$health.deviceError }

    $detail = 'ok=' + $okVal + ' native=' + $nativeVal + ' deviceOpen=' + $devVal + $errText

    # 指纹：本桥接 health 固定带 native / deviceOpen / pluginError / deviceError 字段；
    # 5199 若被别的服务占用，即使回 ok=true 也不能当成本驱动桥接。
    $propNames = @($health.PSObject.Properties | ForEach-Object { $_.Name })
    $ours = ($propNames -contains 'native') -and ($propNames -contains 'deviceOpen')

    if (-not $ours) {
        EmitUnknown 'V6' ($detail + '（响应缺少 native/deviceOpen 字段，疑似非本驱动桥接，不判 PASS）')
    } elseif ($null -ne $okVal -and $null -ne $nativeVal -and $null -ne $devVal -and
              $okVal -eq $true -and $nativeVal -eq $true -and $devVal -eq $true -and $errText -eq '') {
        EmitPass 'V6' $detail
    } elseif ($okVal -eq $true) {
        EmitFail 'V6' ($detail + '（ok 为真但 native/deviceOpen 未全真或存在错误字段）')
    } else {
        EmitFail 'V6' $detail
    }
}

# -----------------------------------------------------------------------------
# 汇总与退出码
# -----------------------------------------------------------------------------
$passCount = 0
$failCount = 0
$unknownCount = 0
foreach ($r in $script:Results) {
    if ($r.Level -eq 'PASS') { $passCount = $passCount + 1 }
    elseif ($r.Level -eq 'FAIL') { $failCount = $failCount + 1 }
    else { $unknownCount = $unknownCount + 1 }
}

$elapsed = [math]::Round(((Get-Date).Ticks - $script:StartTicks) / 10000000.0, 2)

Write-Host ''
Write-Host ('--- 合计 PASS=' + $passCount + ' FAIL=' + $failCount + ' UNKNOWN=' + $unknownCount + ' 用时=' + $elapsed + 's ---')

if ($failCount -gt 0) { exit 1 }
if ($unknownCount -gt 0) { exit 2 }
exit 0
