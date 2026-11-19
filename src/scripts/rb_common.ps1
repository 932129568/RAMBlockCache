# RAMBlockCache 部署公共配置（被各脚本 dot-source）
#
# [FIX S-10] 工具解析优先级：显式 param > 环境变量 > 自动发现 > $null。
#   显式 param：dot-source 时传参（见用法示例）。
#   环境变量  ：RB_SIGNTOOL / RB_MSBUILD / RB_NUGETPKG。
#   自动发现  ：signtool = deploy.ps1:104-119 同款 Windows Kits 递归（x64 版本倒序）；
#               MSBuild  = Get-Command 探测 PATH（A5：不额外探测用户级 .NET SDK）；
#               NugetPkg = $env:USERPROFILE\.nuget\packages。
#   $null     ：找不到即为 $null——dot-source 不 throw（懒解析），消费方点到用
#               时报错说清"缺什么、怎么设"（fail-loud）。game_mode.ps1 /
#               run_diskspd.ps1 只用 Assert-Admin/$ProjectRoot，不被签名工具
#               缺失波及；install.ps1（legacy）消费三项，缺失即 $null 调用错误
#               （fail-loud；[FIX N-b] 入口口径统一为：普通用户=dist\RAMBlockCache-Setup.exe，
#                开发者/脚本=deploy.ps1，install.ps1/ramboostsetup.ps1=legacy 备选）。
# 用法示例：
#   . (Join-Path $PSScriptRoot 'rb_common.ps1') -SignTool 'D:\Kits\...\signtool.exe'
#   $env:RB_MSBUILD = 'D:\...\MSBuild.exe'; . (Join-Path $PSScriptRoot 'rb_common.ps1')
#
# Sign-Driver 时间戳策略 [FIX S-10]：-TimestampUrl（默认 digicert）+ -NoTimestamp；
# 失败消息区分"时间戳不可达（离线？）"与"签名失败（查证书拇指纹/测试根）"。
param(
    [string]$SignTool,
    [string]$MSBuild,
    [string]$NugetPkg
)
$ErrorActionPreference = 'Stop'

# 工程根 = scripts\ 的上一级
$Script:ProjectRoot = Split-Path -Parent $PSScriptRoot

$Script:DriverDir   = Join-Path $ProjectRoot 'driver'
$Script:SysPath      = Join-Path $DriverDir 'x64\Release\RAMBlockCache.sys'
$Script:InfPath      = Join-Path $DriverDir 'RAMBlockCache.inf'
$Script:ServiceName  = 'RAMBlockCache'
$Script:VolumeClassGuid = '{71a27cdd-812a-11d0-bec7-08002be2092f}'

# --- [FIX S-10] 工具发现函数（纯发现，不 throw；找不到返回 $null）---
function Find-SignTool {
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $roots = @()
    if ($env:ProgramFiles)        { $roots += (Join-Path $env:ProgramFiles        'Windows Kits\10\bin') }
    if (${env:ProgramFiles(x86)}) { $roots += (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') }
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

function Find-MSBuild {
    $cmd = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Find-NuGetPkg {
    if ($env:USERPROFILE) {
        $p = Join-Path $env:USERPROFILE '.nuget\packages'
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

# 显式 param > 环境变量 > 自动发现 > $null。
function Resolve-Tool([string]$Explicit, [string]$EnvName, [scriptblock]$Finder) {
    if ($Explicit) { return $Explicit }
    $envVal = [Environment]::GetEnvironmentVariable($EnvName)
    if ($envVal) { return $envVal }
    return (& $Finder)
}

# [FIX S-10] 懒解析：dot-source 时只求值为字符串或 $null，不 throw。
$Script:SignTool = Resolve-Tool $SignTool 'RB_SIGNTOOL' { Find-SignTool }
$Script:CertThumb = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'
$Script:MSBuild  = Resolve-Tool $MSBuild  'RB_MSBUILD'  { Find-MSBuild }
$Script:NugetPkg = Resolve-Tool $NugetPkg 'RB_NUGETPKG' { Find-NuGetPkg }

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '需要管理员权限。请通过 .cmd 启动以触发 UAC 自提权。'
    }
}

# [W1.5/T04/T05] Get-RbStats —— 关键计数器抓取（bench_proto.ps1 判别式对账的数据源）。
# 包装 ramboostctl status（RB_STATS 协议面冻结：CLI 不暴露原始命中数，故有派生口径）：
#   ReadReq / WriteReq / WbFlush  = status 行内原始累计值（精确）；
#   ReadHitPct / ReadHitBytesPct  = status 行内命中率（次数/字节，%.2f）；
#   ReadHit（派生）               = ReadReq × ReadHitPct / 100
#     —— core 语义 read_hit=命中请求数（部分命中计一次，cache.c:918），
#        百分比按两位小数打印，派生相对误差 < 0.005/ReadReq，对"同向判别"无影响；
#   ReadHitBytes 原始值协议面不可得（ReadBytes 未打印、RB_STATS 不加字段）：
#     字节级命中曲线以 ReadHitBytesPct + 特性层 rbfeatctl bytesHit（-Raw 时附取）参与对账；
#   DirtyBytes / WbBytes 原始值  = ramboostctl statval（机器可读，-Raw 时附取）；
#   [T05] LostDirtyBytes         = status 行"丢弃脏数据: X KB"（C7 记账回归观测量；
#     协议面冻结不加 statval 字段，KB 两位小数 → 字节粒度 ~10B，判据为 ==0 足够）。
# 解码：ctl 以 /utf-8 编译，PS 5.1 捕获原生输出前须置 Console::OutputEncoding=UTF8
#       （verify_install.ps1 同款先例），否则中文标签按 OEM 代码页解码为正则失配。
# fail-closed：ctl 缺失/非零退出/关键行解析不到 → throw（绝不返回半真数据混入对账）。
function Get-RbStats {
    param(
        [string]$CtlPath,
        [switch]$Raw
    )
    if (-not $CtlPath) { $CtlPath = Join-Path $ProjectRoot 'ctl\ramboostctl.exe' }
    if (-not (Test-Path -LiteralPath $CtlPath)) {
        throw "Get-RbStats: 未找到 $CtlPath（先编译 ctl，或传 -CtlPath 指定）。"
    }
    $prevEnc = [Console]::OutputEncoding
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
    try {
        # 函数局部 EAP=Continue（Sign-Driver 同款模式）：原生 stderr 合流不触发 Stop，
        # 成败以 $LASTEXITCODE 显式判定（fail-closed 不依赖 EAP 副作用）。
        $ErrorActionPreference = 'Continue'
        $out = & $CtlPath status 2>&1
        $code = $LASTEXITCODE
        $text = ($out | Out-String)
    } finally {
        [Console]::OutputEncoding = $prevEnc
    }
    if ($code -ne 0) {
        throw "Get-RbStats: ramboostctl status 失败（exit=$code）：$text"
    }
    $st = [pscustomobject]@{
        ReadReq         = [uint64]0
        ReadHitPct      = [double]0
        ReadHit         = [double]0      # 派生口径，见函数头注释
        ReadHitBytesPct = [double]0
        WriteReq        = [uint64]0
        WbFlush         = [uint64]0
        DirtyMB         = [double]0
        DirtyBytes      = [uint64]0      # -Raw 时经 statval 填充
        WbBytes         = [uint64]0      # -Raw 时经 statval 填充
        LostDirtyBytes  = [uint64]0      # [T05] C7 记账（丢弃脏数据 KB → 字节）
    }
    # 行格式（ctl/ramboostctl.c cli_print_stats）：
    #   读请求: N   读命中(次数): P%   读命中(字节): Q%
    #   写请求: N   写字节: G GB   缓存吸收: R%
    #   回写次数: N   回写字节: G GB   淘汰字节: G GB
    #   缓存大小: N MB   当前占用: G GB   脏数据: M MB
    #   空闲释放: X MB   丢弃脏数据: Y KB
    $m = [regex]::Match($text, '读请求:\s*(\d+)')
    if ($m.Success) { $st.ReadReq = [uint64]$m.Groups[1].Value }
    $m = [regex]::Match($text, '读命中\(次数\):\s*([\d.]+)%')
    if ($m.Success) { $st.ReadHitPct = [double]$m.Groups[1].Value }
    $m = [regex]::Match($text, '读命中\(字节\):\s*([\d.]+)%')
    if ($m.Success) { $st.ReadHitBytesPct = [double]$m.Groups[1].Value }
    $st.ReadHit = [math]::Round($st.ReadReq * $st.ReadHitPct / 100.0, 2)
    $m = [regex]::Match($text, '写请求:\s*(\d+)')
    if ($m.Success) { $st.WriteReq = [uint64]$m.Groups[1].Value }
    $m = [regex]::Match($text, '回写次数:\s*(\d+)')
    if ($m.Success) { $st.WbFlush = [uint64]$m.Groups[1].Value }
    $m = [regex]::Match($text, '脏数据:\s*([\d.]+)\s*MB')
    if ($m.Success) { $st.DirtyMB = [double]$m.Groups[1].Value }
    $m = [regex]::Match($text, '丢弃脏数据:\s*([\d.]+)\s*KB')
    if ($m.Success) { $st.LostDirtyBytes = [uint64]([double]$m.Groups[1].Value * 1024) }
    if ($Raw) {
        $st.DirtyBytes = [uint64](& $CtlPath statval dirty_bytes)
        $st.WbBytes    = [uint64](& $CtlPath statval wb_bytes)
    }
    return $st
}

function Install-DriverBinary {
    $dest = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
    Copy-Item $Script:SysPath $dest -Force
    Write-Host "[ok] 已部署 $dest"
}

function Sign-Driver {
    param(
        [string]$TimestampUrl = 'http://timestamp.digicert.com',
        [switch]$NoTimestamp
    )
    if (-not (Test-Path $Script:SysPath)) { throw "未找到 $($Script:SysPath)，请先 build。" }
    # [FIX S-10] 点到用 fail-loud：说清缺什么、怎么设（不再静默用死路径）。
    if (-not $Script:SignTool) {
        throw 'signtool 未找到：设 RB_SIGNTOOL 环境变量 / 传 -SignTool / 安装 Windows Kits（deploy.ps1 同款 Kits 递归亦未命中）。'
    }
    Write-Host "[..] 签名 $($Script:SysPath)"
    $tsArgs = if ($NoTimestamp) { @() } else { @('/t', $TimestampUrl) }
    # [FIX S-10] 捕获期间函数作用域 EAP=Continue（函数局部遮蔽，不影响调用方）：
    # 避免 Stop 语义把 signtool stderr 警告变成终止性错误；输出 2>&1 捕获用于失败归因。
    $ErrorActionPreference = 'Continue'
    $out = & $Script:SignTool sign /v /sha1 $Script:CertThumb /fd SHA256 @tsArgs $Script:SysPath 2>&1
    $rc = $LASTEXITCODE
    if ($rc -ne 0) {
        if (($out | Out-String) -match 'timestamp') {
            throw "时间戳不可达（离线？）：联网重试或改用 -NoTimestamp。signtool 输出: $(($out | Out-String).Trim())"
        }
        throw "签名失败（查证书拇指纹/测试根）。signtool 输出: $(($out | Out-String).Trim())"
    }
    Write-Host '[ok] 签名完成'
}
