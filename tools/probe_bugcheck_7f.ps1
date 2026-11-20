# ===================================================================
#  probe_bugcheck_7f.ps1  [296 2026-11-19 v3]
#
#  0x7F / Arg1=0x8 (double fault) 内核崩溃取证。
#
#  v1 失败原因（保留在此，避免重犯）：
#    (a) 只传 -y C:\Symbols，未传 -i image path => cdb 打印
#        "Executable search path is:"（空）且
#        "Unable to load image \SystemRoot\system32\ntoskrnl.exe,
#         Win32 error 0n2" => 无符号 => !analyze 给出
#        MODULE_NAME: nt_wrong_symbols / FAILURE_BUCKET_ID: WRONG_SYMBOLS_*
#        这两个值对判罪毫无意义。必须走 srv* 在线符号 + 本地 image path。
#    (b) "{0,-20s} = {1}" 不是合法 .NET 格式串（要 {0,-20}），
#        抛 "Error formatting a string" 直接中断整个分析。
#    (c) BugCheck 解析只认 "BugCheck 7f, {...}" 一种形态，
#        而 !analyze -v 实际输出的是 "Arg1: 0000..., EXCEPTION_DOUBLE_FAULT"
#        + "BUGCHECK_CODE: 7f" + "BUGCHECK_P1..P4"，故全部 MISS。
#
#  v2 新增（这是判罪的关键，不是锦上添花）：
#    - 无符号栈归属：STACK_TEXT / kv 里的每个帧地址直接与 lm 区间比对，
#      不给符号也能回答"栈落在哪个模块"—— 空符号集不得判 PASS。
#    - !blackboxntfs / !blackboxbsd / !blackboxpnp：
#      dump 自带崩溃前 NTFS / 电源 / PnP 操作黑匣子，
#      直接回答"崩溃那一刻系统在做什么文件操作"。
#    - RC 语义：0=分析完成 1=非提权 2=无 dump 3=无调试器 4=符号缺失未定罪
#
#  v3 修正（v2 实测 37 分钟无结果，根因就在第一条）：
#    (a) `.reload /f` 不带模块名 = 强制重载**全部**已加载模块，会给这台机器上
#        每一个驱动逐个联网要符号。实测只推进到第 19 个（gna.sys /
#        TeeDriverW10x64.sys / Netwaw20.sys...），而 ntkrnlmp.pdb 早在
#        17:19 就已落盘本地（11,563,008 B）。改为 `.reload /f nt`（只内核）。
#    (b) 新增 Pass B0：**零符号零网络**读 _EXCEPTION_RECORD（dq P3）与寄存器
#        （.trap P2 + r）。这才是 double fault 唯一可靠且秒级的入口 ——
#        P4 是 KiTrap 自己（报告者），P3 的 +0x10 才是真凶地址。
#        Pass B0 默认就跑；完整符号分析改成 -Full 显式开启。
#
#  READ-ONLY: 不删文件、不写注册表、不碰驱动。
# ===================================================================

[CmdletBinding()]
param(
    [string]$OutDir = 'C:\ProgramData\rb-deploy',
    [string]$DumpPath = '',
    [switch]$AllDumps,
    [switch]$Offline,
    [switch]$Full
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function W([string]$s) { Write-Output $s }

$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Output '[FAIL] not elevated - minidump is Administrators-only (Win32 error 0n5).'
    Write-Output '       run tools\probe_bugcheck_7f.cmd (self-elevating) instead.'
    exit 1
}

if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$reportPath = Join-Path $OutDir "bugcheck-7f-$stamp.txt"
$lines = [System.Collections.Generic.List[string]]::new()
function Emit([string]$s) { $lines.Add($s); W $s }
function Head([string]$s) {
    Emit ''
    Emit ('=' * 74)
    Emit "  $s"
    Emit ('=' * 74)
}

Emit "[run] $stamp  user=$($identity.Name)  elevated=YES  offline=$($Offline.IsPresent)"

# -------------------------------------------------------------------
# 0) 摘出模块加载区间（无符号栈归属的基础）
# -------------------------------------------------------------------
function Get-ModuleRanges([string]$text) {
    # lm 输出形如：
    #   fffff805`7cb70000 fffff805`7cbe5000   RAMBlockCache   (deferred)
    #
    # 【必须踩过的坑】不能只写 `(\S+)` 当模块名。STACK_TEXT 的续行是
    #   ffffa580`f1728d28 fffff805`ea36fea9     : 00000000`0000007f ...
    # 它同样以 "两个 16-hex 地址" 开头，第三个字段恰好是 ":"，
    # 于是会造出一个 ffffa580f1728d50 - fffff805ea00f6a6 的**巨大假区间**，
    # 把后续所有栈帧全吞进去 —— 实测就是它让归属表里冒出名为 ":" 的模块，
    # 并且 RAMBlockCache 的判定被污染。
    #
    # 判据：模块名必须是合法标识符（字母/数字/下划线/点/连字符，无冒号），
    # 且区间跨度不得超过 256 MB（Kernel=0xFFFFFFFFF 明显越界）。
    $map = @{}
    foreach ($m in [regex]::Matches($text, '(?im)^([0-9a-f]{8}`[0-9a-f]{8})\s+([0-9a-f]{8}`[0-9a-f]{8})\s+([A-Za-z_][A-Za-z0-9_.\-]*)\b')) {
        $s = [Convert]::ToUInt64(($m.Groups[1].Value -replace '`', ''), 16)
        $e = [Convert]::ToUInt64(($m.Groups[2].Value -replace '`', ''), 16)
        $n = $m.Groups[3].Value
        if ($e -le $s) { continue }
        if (($e - $s) -gt [uint64]268435456) { continue }   # > 256 MB：不是真实模块
        $map[$n.ToLower()] = @{ Start = $s; End = $e; Name = $n }
    }
    return $map
}

function Resolve-Address([uint64]$addr, $map) {
    foreach ($k in $map.Keys) {
        $r = $map[$k]
        if ($addr -ge $r.Start -and $addr -lt $r.End) {
            return @{ Module = $r.Name; Offset = ($addr - $r.Start) }
        }
    }
    return $null
}

# -------------------------------------------------------------------
# 1) 选 dump
# -------------------------------------------------------------------
Head '1. dump'

$all = @(Get-ChildItem 'C:\Windows\Minidump' -Filter '*.dmp' -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending)
if ($all.Count -eq 0) { Emit '[FAIL] C:\Windows\Minidump 无 .dmp'; exit 2 }
foreach ($d in $all) {
    Emit ("  {0}  {1,10:N0} B  {2:yyyy-MM-dd HH:mm:ss}" -f $d.Name, $d.Length, $d.LastWriteTime)
}

$targets = @()
if ($DumpPath) {
    $one = Get-Item -LiteralPath $DumpPath -ErrorAction SilentlyContinue
    if (-not $one) { Emit "[FAIL] 指定 dump 不存在: $DumpPath"; exit 2 }
    $targets = @($one)
} elseif ($AllDumps) {
    $targets = $all
} else {
    # 默认只跑最新一份：符号下载是最贵的动作，不该对 5 份各做一次
    $targets = @($all[0])
}
Emit "[pick] $($targets.Count) 份: $(($targets | ForEach-Object { $_.Name }) -join ', ')"

# -------------------------------------------------------------------
# 2) 调试器
# -------------------------------------------------------------------
Head '2. 调试器'
$cdb = @('C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe',
         'C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe') |
       Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $cdb) { Emit '[FAIL] 未找到 cdb.exe'; exit 3 }
Emit "[ok] cdb = $cdb"

$symDir = 'C:\Symbols'
if (-not (Test-Path -LiteralPath $symDir)) { New-Item -ItemType Directory -Path $symDir -Force | Out-Null }
# v3：默认只走**本地缓存**，零网络。srv* 只在 -Full 且未 -Offline 时才挂。
# 理由见文件头 v3 (a)：v2 的 srv* + .reload /f 组合让一次取证跑了 37 分钟没结果，
# 而 ntkrnlmp.pdb 早已在 C:\Symbols 落地（11,563,008 B）。
$symPath = if ($Offline -or -not $Full) { $symDir }
           else { "srv*$symDir*https://msdl.microsoft.com/download/symbols" }
Emit "[sym] $symPath  (Full=$($Full.IsPresent) Offline=$($Offline.IsPresent))"

# -------------------------------------------------------------------
# 3) 逐份分析
# -------------------------------------------------------------------
$summary = [System.Collections.Generic.List[object]]::new()
$symbolOkOverall = $false

foreach ($d in $targets) {
    Head ("3. " + $d.Name + "  (" + $d.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + ")")

    # ---------------------------------------------------------------
    # Pass A：只取崩溃参数。不下载符号（v1 实测 1.6s），因此零网络成本。
    # 为什么单独一遍：.trap / dq 需要 Arg2/Arg3 的具体地址，
    # 而这两个值只有解析完第一遍输出才知道 —— 鸡生蛋问题，只能两遍。
    # ---------------------------------------------------------------
    $logA = Join-Path $env:TEMP ("cdbA_" + $d.BaseName + ".txt")
    & $cdb @('-z', $d.FullName, '-i', "$env:SystemRoot\System32", '-y', $symDir,
             '-c', '.symfix; !analyze -v; q') 2>&1 |
        Out-File -FilePath $logA -Encoding utf8
    $tA = Get-Content -LiteralPath $logA -Raw -ErrorAction SilentlyContinue
    if (-not $tA) { Emit '[FAIL] Pass A 无输出'; continue }
    if ($tA -match 'Debuggee initialization failed') { Emit '[FAIL] cdb 打不开 dump'; continue }

    $bug = '?'; $p1 = '?'; $p2 = '?'; $p3 = '?'; $p4 = '?'
    if ($tA -match '(?m)^BUGCHECK_CODE:\s+([0-9a-fA-F]+)') { $bug = '0x' + $Matches[1] }
    if ($tA -match '(?m)^BUGCHECK_P1:\s+([0-9a-fA-F]+)')   { $p1  = '0x' + $Matches[1] }
    if ($tA -match '(?m)^BUGCHECK_P2:\s+([0-9a-fA-F]+)')   { $p2  = '0x' + $Matches[1] }
    if ($tA -match '(?m)^BUGCHECK_P3:\s+([0-9a-fA-F]+)')   { $p3  = '0x' + $Matches[1] }
    if ($tA -match '(?m)^BUGCHECK_P4:\s+([0-9a-fA-F]+)')   { $p4  = '0x' + $Matches[1] }
    if ($bug -eq '?') {
        if ($tA -match 'UNEXPECTED_KERNEL_MODE_TRAP\s*\(7f\)') { $bug = '0x7f' }
        foreach ($pair in @(@('Arg1','p1'), @('Arg2','p2'), @('Arg3','p3'), @('Arg4','p4'))) {
            if ($tA -match ("(?m)^" + $pair[0] + ":\s+([0-9a-fA-F]+)")) {
                Set-Variable -Name $pair[1] -Value ('0x' + $Matches[1])
            }
        }
    }
    Emit ("[A] BugCheck=" + $bug + "  P1=" + $p1 + "  P2=" + $p2 + "  P3=" + $p3 + "  P4=" + $p4)

    $is7f = ($bug -match '^0x0*7f$')
    $isDF = ($p1  -match '^0x0*8$')
    Emit ("  判据 0x7F=" + $is7f + "  Arg1=0x8(double fault)=" + $isDF)

    # 【v4 新增】Pass A 本身就带符号（-y C:\Symbols + .symfix），
    # 若日志里出现真实解出的符号名，就说明符号可用 —— 不必等 Pass B。
    # 否则默认路径永远拿不到 $symbolOkOverall，必报假告警并 exit 4。
    if (-not $symbolOkOverall) {
        $aWrong = $tA -match 'Kernel symbols are WRONG|MODULE_NAME:\s+nt_wrong_symbols|FAILURE_BUCKET_ID:\s*WRONG_SYMBOLS'
        $aResolved = ($tA -match '(?m):\s+nt![A-Za-z_]') -or ($tA -match 'RAMBlockCache\+0x[0-9a-f]+')
        if ((-not $aWrong) -and $aResolved) {
            $symbolOkOverall = $true
            Emit '  [sym] Pass A 已解出符号名 => 符号可用'
        }
    }

    # ---------------------------------------------------------------
    # Pass B0：零符号、零网络。这才是 double fault 的正确入口。
    #
    # 【v3 关键修正 —— 这是 v2 跑了 37 分钟没出结果的原因】
    # v2 用 `.reload /f`：`/f` 是**强制重载全部模块**，会给这台机器上
    # 每一个已加载驱动逐个去符号服务器要符号。实测 37 分钟只推进到第 19 个
    # 模块（gna.sys / TeeDriverW10x64.sys / Netwaw20.sys ...），
    # 每个都先 "Unable to load image ... Win32 error 0n2"（它们在 DriverStore，
    # 而 -i 只给了 System32）再联网超时。而 ntkrnlmp.pdb 早在 17:19 就已
    # 落地本地（11,563,008 B），等于在反复找一件已经在自己手里的东西。
    #
    # P3 指向的 _EXCEPTION_RECORD 是**纯内存读取**，dq 不需要任何符号；
    # .trap + r 也不需要。因此先做这一遍，秒级出结果。
    # ---------------------------------------------------------------
    $exrAddrRaw = $null
    if ($p3 -match '^0x[0-9a-f]+$') {
        $logB0 = Join-Path $env:TEMP ("cdbB0_" + $d.BaseName + ".txt")
        $c0 = @("dq $p3 L8")
        if ($p2 -match '^0x[0-9a-f]+$') { $c0 += ".trap $p2" }
        # 不用 .echo 做标记：dq 的解析只认「行首两个 16-hex 地址」，
        # 不依赖任何标记；少一条不确定的调试器命令就少一个失败点。
        $c0 += @('r', 'q')
        $sw0 = [System.Diagnostics.Stopwatch]::StartNew()
        # -y 只给本地缓存，不带 srv* ⇒ 零网络。
        & $cdb @('-z', $d.FullName, '-i', "$env:SystemRoot\System32", '-y', $symDir,
                 '-c', ($c0 -join '; ')) 2>&1 |
            Out-File -FilePath $logB0 -Encoding utf8
        $sw0.Stop()
        $t0 = Get-Content -LiteralPath $logB0 -Raw -ErrorAction SilentlyContinue
        # 注意：`-f` 不能换行到下一行（PowerShell 把换行当语句结束），必须同行。
        Emit ("[B0] elapsed={0:N1}s (zero-symbol, zero-network)  log={1}" -f $sw0.Elapsed.TotalSeconds, $logB0)

        if ($t0) {
            # dq 输出形如：ffffc688`dd0e8fe0  00000000`c0000005 00000000`00000000 ...
            $rows = @()
            foreach ($ln in ($t0 -split "`r?`n")) {
                $m3 = [regex]::Match($ln, '^\s*([0-9a-f]{8}`[0-9a-f]{8})\s+((?:[0-9a-f]{8}`[0-9a-f]{8}\s*){1,4})')
                if ($m3.Success) {
                    $words = @()
                    foreach ($w in ($m3.Groups[2].Value -split '\s+')) {
                        if ($w -match '^[0-9a-f]{8}`[0-9a-f]{8}$') { $words += $w }
                    }
                    $rows += , @($m3.Groups[1].Value, $words)
                }
            }
            if ($rows.Count -ge 1 -and $rows[0][1].Count -ge 4) {
                $base = [Convert]::ToUInt64(($rows[0][0] -replace '`',''),16)
                $vals = @()
                foreach ($r in $rows) { foreach ($w in $r[1]) { $vals += $w } }
                Emit ("  _EXCEPTION_RECORD @0x" + ('{0:x}' -f $base) + "  读出 " + $vals.Count + " 个 qword")
                # x64 布局：+0x00 ExceptionCode(+Flags 高 32) / +0x08 ExceptionRecord
                #            +0x10 ExceptionAddress / +0x18 NumberParameters
                # 注：Code 与 Flags 各占 4 字节，合在一个 qword 里。
                if ($vals.Count -ge 3) {
                    $code = $vals[0]
                    $exAddr = $vals[2]
                    Emit ("  ExceptionCode    = 0x" + $code + "  (Flags=0x" + $vals[0].Substring($vals[0].IndexOf('`')+1) + ")")
                    Emit ("  ExceptionAddress = 0x" + $exAddr)
                    Emit ("  NumberParameters = 0x" + $(if ($vals.Count -ge 4) { $vals[3] } else { '?' }))
                    if ($exAddr -match '^[0-9a-f]{8}`[0-9a-f]{8}$') {
                        $exrAddrRaw = [Convert]::ToUInt64(($exAddr -replace '`',''),16)
                        # 【判据不得空跑】ExceptionAddress=0 意味着这份 dump 的
                        # _EXCEPTION_RECORD 没有记录真凶（double fault 二次异常时常见），
                        # 此时**不得**把 0 拿去查 lm 区间，也不得据此脱罪。
                        if ($exrAddrRaw -eq 0) {
                            Emit '  [!] ExceptionAddress = 0 —— 本 dump 的异常记录无真凶地址，'
                            Emit '      不能据此定罪也不能脱罪；改用 STACK_TEXT 栈底帧 + .pdata 归属。'
                            $exrAddrRaw = $null
                        } elseif ($vals.Count -ge 5 -and $vals[4] -match '^[0-9a-f]{8}`[0-9a-f]{8}$') {
                            Emit ("  Information[0]   = 0x" + $vals[4] + "  (double fault 时为 0，或用 P2 直接看 trap frame)")
                        }
                    }
                }
            } elseif ($t0 -match '(?m)^\s*[0-9a-f]{8}`[0-9a-f]{8}\s+\?{8}`\?{8}') {
                # 【v4 区分两种失败，否则会把「dump 没收录这段内存」误报成「脚本解析失败」】
                # 实测 18:48 那次：dq 前 3 行全是 ????????`????????，后 2 行是 0。
                # 这是 minidump 只保存了部分内存（Mini Kernel Dump 的固有裁剪），
                # 不是正则不匹配。两者的处置完全不同：前者是数据不存在，后者可修脚本。
                Emit '  [!] _EXCEPTION_RECORD 内存**未收录**进本 dump（dq 返回 ????????????????）'
                Emit '      => 拿不到 ExceptionAddress。这是 dump 内容缺失，不是解析失败，改脚本也读不出。'
                Emit '      不得据此脱罪：元凶只能靠 STACK_TEXT 栈底帧 + .pdata 归属 + 栈用量推算。'
            } else {
                Emit '  [!] dq 输出未能解析出 _EXCEPTION_RECORD —— 见日志原文'
            }
            # 寄存器现场（.trap 生效后）
            foreach ($rl in ($t0 -split "`r?`n")) {
                if ($rl -match '^(rax|rbx|rcx|rdx|rsi|rdi|rip|rsp|rbp)=\s*([0-9a-f`]+)') {
                    Emit ("  " + $Matches[1] + " = 0x" + $Matches[2])
                }
            }
        }
    } else {
        Emit '  [!] P3 无效，跳过 B0'
    }

    # ---------------------------------------------------------------
    # Pass B：带符号的完整分析。**只在 -Full 时才跑**。
    # 关键修正：`.reload /f nt` 只重载 nt，不是 `.reload /f` 的全部模块。
    # ---------------------------------------------------------------
    if (-not $Full) {
        Emit '  [B] 跳过带符号完整分析（默认）。需要 !analyze -v / blackbox 时加 -Full。'
        if ($exrAddrRaw) {
            $mods0 = @{}
            Emit "  >>> 真凶地址 = 0x$('{0:x}' -f $exrAddrRaw)（无 lm 表，需 -Full 才能归属模块）"
        }
        $summary.Add([pscustomobject]@{
            Dump = $d.Name; Time = $d.LastWriteTime; Bug = $bug; P1 = $p1
            Tail = $(if ($p4 -match '([0-9a-f]{4})$') { $Matches[1].ToLower() } else { '?' })
            SymbolOk = $false; RbOnStack = $false; Attrib = @{}
        })
        continue
    }
    # 【v3 修正】`.reload /f` 不带模块名 = 强制重载**全部**已加载模块，
    # 会为机器上每一个驱动逐个去符号服务器要符号（v2 实测 37 分钟仍卡在第 19 个）。
    # 这里只重载 nt，因为定罪只需要内核符号 + 栈归属。
    $cmdB = @('.symfix', '.reload /f nt', '!analyze -v')
    if ($p2 -match '^0x[0-9a-f]+$') { $cmdB += ".trap $p2" }
    if ($p3 -match '^0x[0-9a-f]+$') { $cmdB += "dq $p3 L8" }
    if ($p2 -match '^0x[0-9a-f]+$') { $cmdB += "dq $p2 L8" }
    $cmdB += @('!blackboxntfs', '!blackboxbsd', '!blackboxpnp', 'kv', 'lm', 'q')
    $cmds = $cmdB -join '; '

    $raw = Join-Path $env:TEMP ("cdb_" + $d.BaseName + ".txt")
    $argList = @(
        '-z', $d.FullName,
        '-i', "$env:SystemRoot\System32",
        '-y', $symPath,
        '-c', $cmds
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $cdb @argList 2>&1 | Out-File -FilePath $raw -Encoding utf8
    $rc = $LASTEXITCODE
    $sw.Stop()
    Emit ("[cdb] rc={0} elapsed={1:N1}s  log={2}" -f $rc, $sw.Elapsed.TotalSeconds, $raw)

    $txt = Get-Content -LiteralPath $raw -Raw -ErrorAction SilentlyContinue
    if (-not $txt) { Emit '[FAIL] 日志为空'; continue }
    if ($txt -match 'Debuggee initialization failed') { Emit '[FAIL] cdb 打不开 dump'; continue }

    # 崩溃参数已在 Pass A 取得，这里不再重复解析（v1 就错在只认一种格式）

    # ---- 3.2 符号是否真的加载成功（这是判罪前提，不得空跑）----
    # 【v4 修正】$symbolOkOverall 只在 Pass B 里被赋值，而 -Full 是可选路径。
    # 默认（不带 -Full）时它永远是 $false ⇒ 结尾必报「符号未加载」+ exit 4。
    # 18:48 那次实测就是如此：日志里明明有 nt!KiDoubleFaultAbort+0x361 与
    # RAMBlockCache+0xe00a 两个已解析符号名，却报 WRONG_SYMBOLS。
    # 判据改为「看日志里有没有真实解出的符号名」，且 Pass A 也算数。
    $wrongSym = $txt -match 'Kernel symbols are WRONG|MODULE_NAME:\s+nt_wrong_symbols|FAILURE_BUCKET_ID:\s*WRONG_SYMBOLS'
    $resolvedInLog = ($txt -match '(?m)^\S+:\s+nt![A-Za-z_]' ) -or
                     ($txt -match 'RAMBlockCache\+0x[0-9a-f]+') -or
                     ($txt -match '(?m)^SYMBOL_NAME:\s+nt!')
    $symbolOk = (-not $wrongSym) -and $resolvedInLog
    Emit "  符号可用 = $symbolOk  (日志中已解出符号名=$resolvedInLog, WRONG_SYMBOLS标记=$wrongSym)"
    if ($symbolOk) { $symbolOkOverall = $true }

    # ---- 3.3 归因字段 ----
    foreach ($k in @('PROCESS_NAME', 'MODULE_NAME', 'IMAGE_NAME', 'FAILURE_BUCKET_ID', 'SYMBOL_NAME')) {
        $h = [regex]::Match($txt, "(?m)^$k\s*:\s*(.+)$")
        if ($h.Success) { Emit ("  " + $k.PadRight(20) + " = " + $h.Groups[1].Value.Trim()) }
    }

    # ---- 3.4 模块区间表 ----
    $mods = Get-ModuleRanges $txt
    Emit "  lm 解析到 $($mods.Count) 个模块区间"

    $ntBase = $null
    if ($txt -match '(?m)^Kernel base = 0x([0-9a-f`]+)') { $ntBase = [Convert]::ToUInt64(($Matches[1] -replace '`',''),16) }

    # ---- 3.5 无符号栈归属 ----
    # 教训（v2 首版实测踩到）：STACK_TEXT 行形如
    #   ffffa580`f1728d28 fffff805`ea36fea9 : <args> : nt+0x4fecc0
    # 续行里"分隔符 :"后面才跟模块注释。早期正则把 `:` 当成了模块名，
    # 于是归属表里出现一个名为 ":" 的假模块。这里只取**帧地址**
    # （每行前两个 16-hex 里的第二个 = 返回地址），模块名一律靠 lm 区间反查，
    # 不采信日志里的文本注释 —— 注释会撒谎，区间不会。
    $frames = [System.Collections.Generic.List[uint64]]::new()
    $inStack = $false
    foreach ($ln in ($txt -split "`r?`n")) {
        if ($ln -match '^\s*STACK_TEXT:') { $inStack = $true; continue }
        if ($inStack) {
            if ($ln -match '^\s*$' -or $ln -match '^\s*[A-Z_]{4,}:') { $inStack = $false; continue }
            $m = [regex]::Match($ln, '^([0-9a-f]{8}`[0-9a-f]{8})\s+([0-9a-f]{8}`[0-9a-f]{8})')
            if ($m.Success) {
                $frames.Add([Convert]::ToUInt64(($m.Groups[2].Value -replace '`',''),16))
            }
        }
    }
    # kv 段：每行以 sp 开头，第二列是返回地址
    $inKv = $false
    foreach ($ln in ($txt -split "`r?`n")) {
        if ($ln -match '^\s*ChildEBP\s+RetAddr' -or $ln -match '^\s*Child-SP\s+RetAddr') { $inKv = $true; continue }
        if ($inKv) {
            if ($ln -match '^\s*$') { $inKv = $false; continue }
            $m = [regex]::Match($ln, '^([0-9a-f]{8}`[0-9a-f]{8})\s+([0-9a-f]{8}`[0-9a-f]{8})')
            if ($m.Success) {
                $frames.Add([Convert]::ToUInt64(($m.Groups[2].Value -replace '`',''),16))
            }
        }
    }

    $uniq = @($frames | Sort-Object -Unique)
    Emit "  栈帧地址（去重）= $($uniq.Count)"

    $attrib = @{}
    $unresolved = 0
    foreach ($fa in $uniq) {
        # 0xffff800000000000 写成十进制：PowerShell 会把该十六进制字面量
        # 解释成负 Int64，再 [uint64] 强制转换就抛 InvalidArgument。
        # 与本仓 Get-FlagsInt 的 0xffffffff 坑同源。
        if ($fa -lt [uint64]18446603336221196288) { $unresolved++; continue }
        $r = Resolve-Address $fa $mods
        if ($r) {
            $key = $r.Module.ToLower()
            if (-not $attrib.ContainsKey($key)) { $attrib[$key] = 0 }
            $attrib[$key]++
        } else { $unresolved++ }
    }
    if ($attrib.Count -eq 0) {
        Emit '  [FAIL] 无任何栈帧能归属到模块 —— 判据空跑，不得据此定罪或脱罪'
    } else {
        Emit '  --- 栈帧模块归属（按 lm 区间反查，不采信文本注释）---'
        foreach ($k in ($attrib.Keys | Sort-Object { -$attrib[$_] })) {
            Emit ("    " + $k.PadRight(32) + " " + $attrib[$k] + " 帧")
        }
        Emit "  未能归属的帧 = $unresolved"
    }

    $rbOnStack = $attrib.ContainsKey('ramblockcache')
    Emit "  >>> RAMBlockCache 出现在栈中 = $rbOnStack"

    # 360 文件过滤驱动（R9 相关，必须单列）
    $avStack = @($attrib.Keys | Where-Object { $_ -match '^360' })
    Emit ("  >>> 360 系列出现在栈中 = " + $(if ($avStack.Count) { ($avStack -join ', ') } else { 'NO' }))

    # ---- 3.6 Arg4 的正确读法 + EXCEPTION_RECORD 的真凶地址 ----
    # 【必须记住】0x7F 的 P4 是 KiTrap 自己处理 double fault 的位置，
    # 它几乎总是落在 nt 内，**指向的是"报告者"不是"肇事者"**。
    # v1 把它当元凶归属是错的，这里显式标注语义，避免再次误读。
    if ($p4 -match '^0x[0-9a-f]+$' -and $p4 -ne '0x0') {
        $a4v = [Convert]::ToUInt64($p4.Substring(2), 16)
        $r4 = Resolve-Address $a4v $mods
        $where = if ($r4) { "$($r4.Module)+0x$('{0:x}' -f $r4.Offset)" }
                 elseif ($ntBase -and $a4v -ge $ntBase) { "nt+0x$('{0:x}' -f ($a4v - $ntBase))" }
                 else { '未能归属' }
        Emit "  Arg4 = $where"
        Emit "         [语义] 0x7F 的 Arg4 是 double fault 处理路径，属「报告者」，不是「肇事者」"
    }

    # 从 dq P3 的输出里取 _EXCEPTION_RECORD 的 ExceptionAddress
    # 布局: +0x00 ExceptionCode / +0x08 ExceptionFlags / +0x10 ExceptionRecord
    #       +0x18 ExceptionAddress / +0x20 NumberParameters
    # （x64 _EXCEPTION_RECORD 实际为：
    #   +0x00 Code, +0x04 Flags, +0x08 Record, +0x10 Address,
    #   +0x18 NumberParameters, +0x20 Information[15]）
    $exrAddr = $null
    $dqSeg = [regex]::Match($txt, '(?s)dq\s+ffff[0-9a-f`]+\s+L8\s*\n(.*?)(?=\nkd>|\n!blackbox|\nquit:)')
    if ($dqSeg.Success) {
        $rows = $dqSeg.Groups[1].Value -split "`r?`n" | Where-Object { $_ -match '^[0-9a-f]{8}`[0-9a-f]{8}\s+[0-9a-f`]{8,}' }
        $vals = @()
        foreach ($rw in $rows) {
            $m2 = [regex]::Match($rw, '^[0-9a-f]{8}`[0-9a-f]{8}\s+([0-9a-f` ]+)$')
            if ($m2.Success) {
                foreach ($w in ($m2.Groups[1].Value -split '\s+')) {
                    if ($w -match '^[0-9a-f`]{8,17}$') { $vals += $w }
                }
            }
        }
        Emit "  dq 读出 $($vals.Count) 个 qword"
        if ($vals.Count -ge 5) {
            # vals[0]=Code, vals[1]=Flags/Record, vals[2]=Record, vals[3]=Address
            $candidate = $vals[3]
            if ($candidate -match '^[0-9a-f`]+$') {
                $v = [Convert]::ToUInt64(($candidate -replace '`',''),16)
                if ($v -ge [uint64]18446603336221196288) { $exrAddr = $v }
            }
            Emit ("  ExceptionCode       = 0x" + $vals[0])
            Emit ("  ExceptionAddress    = 0x" + $vals[3])
            Emit ("  NumberParameters    = 0x" + $vals[4])
        }
    }
    if ($exrAddr) {
        $er = Resolve-Address $exrAddr $mods
        if ($er) {
            Emit "  >>> 真凶地址归属 = $($er.Module)+0x$('{0:x}' -f $er.Offset)"
        } else {
            Emit "  >>> 真凶地址 = 0x$('{0:x}' -f $exrAddr)（未命中 lm 区间）"
        }
    } else {
        Emit '  [!] 未能从 dq 输出确认 ExceptionAddress —— 真凶未定位，禁止定罪'
    }

    # ---- 3.7 黑匣子：崩溃前在做什么 ----
    foreach ($bb in @('blackboxntfs', 'blackboxbsd', 'blackboxpnp')) {
        $idx = $txt.IndexOf("!$bb")
        if ($idx -lt 0) { continue }
        $seg = $txt.Substring($idx, [Math]::Min(4000, $txt.Length - $idx))
        # 截到下一个提示符
        $cut = $seg.IndexOf("`nkd>")
        if ($cut -gt 0) { $seg = $seg.Substring(0, $cut) }
        $probe = ($seg -split "`r?`n" | Where-Object { $_ -match '\S' -and $_ -notmatch "^\s*!$bb\s*$" } | Select-Object -First 24)
        Emit "  --- !$bb ---"
        if ($probe.Count -eq 0) { Emit '    (空)' } else { foreach ($l in $probe) { Emit ('    ' + $l.TrimEnd()) } }
    }

    $tail = '?'
    if ($p4 -match '([0-9a-f]{4})$') { $tail = $Matches[1].ToLower() }

    $summary.Add([pscustomobject]@{
        Dump = $d.Name; Time = $d.LastWriteTime; Bug = $bug; P1 = $p1; Tail = $tail
        SymbolOk = $symbolOk; RbOnStack = $rbOnStack; Attrib = $attrib
    })
}

# -------------------------------------------------------------------
# 4) 横向比对
# -------------------------------------------------------------------
Head '4. 横向比对'
if ($summary.Count -ge 2) {
    $tails = @($summary | ForEach-Object { $_.Tail } | Sort-Object -Unique)
    Emit "  dump=$($summary.Count)  不同 Arg4 末4位=$($tails.Count): $($tails -join ', ')"
    if ($tails.Count -eq 1) { Emit '  [结论] 同一故障点（Arg4 末4位一致），非随机硬件抖动' }
    else { Emit '  [注意] 末4位不一致 —— 可能不同故障点' }
    $rbAll = @($summary | Where-Object { $_.RbOnStack })
    Emit "  栈中出现 RAMBlockCache 的 dump 数 = $($rbAll.Count) / $($summary.Count)"
} else {
    Emit '  仅 1 份 dump，无法横向比对'
}

# -------------------------------------------------------------------
# 5) 现场事实
# -------------------------------------------------------------------
Head '5. 现场事实'
$uf = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}' -ErrorAction SilentlyContinue).UpperFilters
Emit "  卷 UpperFilters = $($uf -join ', ')"
$svc = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache' -ErrorAction SilentlyContinue
if ($svc) {
    $sm = @{ 0 = 'Boot'; 1 = 'System'; 2 = 'Auto'; 3 = 'Demand'; 4 = 'Disabled' }
    Emit "  RAMBlockCache Start = $($svc.Start) ($($sm[[int]$svc.Start]))"
}
$vl = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management' -ErrorAction SilentlyContinue).VerifyDriverLevel
Emit "  VerifyDriverLevel = $vl$(if ($vl -and $vl -ne 0) { '  [事实] 驱动验证器开启' })"
$sec = Get-CimInstance Win32_Service -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '360|QHSafe|ZhuDong|Huorong|Kingsoft|Tencent' }
if ($sec) { foreach ($s in $sec) { Emit "  安全软件: $($s.Name) State=$($s.State) StartMode=$($s.StartMode)" } }
else { Emit '  安全软件: 未发现' }

# -------------------------------------------------------------------
# 6) 落盘
# -------------------------------------------------------------------
Head '6. 结论口径'
if ($symbolOkOverall) {
    Emit '  符号已加载：MODULE_NAME / FAILURE_BUCKET_ID 可用于归因。'
} else {
    Emit '  [WARN] 符号未加载：只有"栈帧区间归属"可用。'
    Emit '         若日志中出现 MODULE_NAME=nt_wrong_symbols / FAILURE_BUCKET_ID=WRONG_SYMBOLS_*，'
    Emit '         那是符号缺失的产物，禁止据此定罪。'
}
Emit '  READ-ONLY：未删文件、未写注册表、未触碰驱动。'

$lines | Out-File -LiteralPath $reportPath -Encoding utf8
W ''
W "报告 = $reportPath"
if ($symbolOkOverall) { exit 0 } else { exit 4 }