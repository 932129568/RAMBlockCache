# =============================================================================
#  RB-l2-enable.ps1  --  open L2 second-level cache on a volume (ELEVATED)
#
#  目的：在 Windows Verifier（Special pool + Pool tracking）开启的状态下，
#        把 L2 二级缓存真正跑起来，为 176 的 P0-A / P0-B / P1-C 取真机证据。
#
#  为什么需要这一步：
#    L2 默认 OFF（Features\Flags=0）。只要它不开，rbL2.c 里的 drop / probe /
#    store / IdxLoad / IdxSave 一条都不会执行 —— 176 改的三条路径就永远停在
#    "静态落地"，无论重启多少次、Verifier 开多久都拿不到证据。
#
#  安全边界（务必读）：
#    * L2 容器是卷根下的一个**新文件** D:\RAMBoostL2c.bin，大小 = -SizeGB。
#      删除该文件 = 删除缓存，**不影响卷上任何既有数据**。
#    * 驱动只从卷读取数据块写入容器，不会修改被缓存的文件。
#    * WriteBack 保持 0（直写）：即使蓝屏，业务数据已在盘上。
#    * 风险仍在：P0-A 是 UAF，若 176 没修干净，Special pool 会立即 BSOD。
#      这正是本步骤要验证的东西 —— 蓝屏 = 有价值的阴性结果，不是意外。
#
#  用法（必须提权）：
#      右键 RB-l2-enable.cmd -> "以管理员身份运行"
#    或
#      pwsh -File RB-l2-enable.ps1 -Letter D -SizeGB 2
# =============================================================================
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z]$')]
    [string]$Letter = 'D',

    [ValidateRange(1, 128)]
    [int]$SizeGB = 2
)

$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'

# ---------------------------------------------------------------------------
# 0. 路径与日志
# ---------------------------------------------------------------------------
$scriptDir = $PSScriptRoot
$repoRoot  = Split-Path -Parent $scriptDir
$ctl       = Join-Path $repoRoot 'src\ctl\rbfeatctl.exe'
$logDir    = 'C:\ProgramData\rb-deploy'
if (-not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$logFile = Join-Path $logDir ('l2-enable-' + $stamp + '.log')

function Say {
    param([string]$Text)
    Write-Host $Text
    Add-Content -LiteralPath $logFile -Value $Text
}

function Fail {
    param([string]$Text, [int]$Code)
    Say ''
    Say ('*** 中止：' + $Text + ' ***')
    Say ('日志：' + $logFile)
    exit $Code
}

$Letter = $Letter.ToUpperInvariant()

Say '============================================================================'
Say (' RAMBlockCache L2 启用   ' + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))
Say (' 目标卷 ' + $Letter + ':   容器 ' + $Letter + ':\RAMBoostL2c.bin   ' + $SizeGB + ' GB')
Say '============================================================================'
Say ''

# ---------------------------------------------------------------------------
# 1. 提权自检
# ---------------------------------------------------------------------------
$isAdmin = $false
try {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    $isAdmin = $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
} catch {
    $isAdmin = $false
}
if (-not $isAdmin) {
    Fail '当前不是管理员。请右键 RB-l2-enable.cmd 选「以管理员身份运行」。' 5
}
Say '[ok] 已提权'

# ---------------------------------------------------------------------------
# 2. 驱动版本闸（关键）
#    只在 176 版驱动上开 L2。旧版驱动没有 P0-A 的 s_Halt 闸，
#    在 Special pool 下开 L2 等于主动制造 UAF 蓝屏。
# ---------------------------------------------------------------------------
$ExpectSys = 'd3a40f4c5e615de16822519165226198dd5684cd4a114ee4954a3b9bbeeb0f66'
$sysPath = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
if (-not (Test-Path -LiteralPath $sysPath)) {
    Fail ('System32 下没有 RAMBlockCache.sys：' + $sysPath) 3
}
$actual = (Get-FileHash -LiteralPath $sysPath -Algorithm SHA256).Hash.ToLowerInvariant()
Say ('[..] System32 sys = ' + $actual)
if ($actual -ne $ExpectSys) {
    Say ('     期望（176 版）= ' + $ExpectSys)
    Fail ('当前驱动的 L2 守卫不是 176 版，拒绝在其上启用 L2。请先完成签名部署与重启。') 4
}
Say '[ok] 驱动是 176 版（含 P0-A / P0-B / P1-C）'

# ---------------------------------------------------------------------------
# 3. 控制器可用性
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $ctl)) {
    Fail ('找不到控制器：' + $ctl) 6
}
Say ('[..] 控制器 = ' + $ctl)

# ---------------------------------------------------------------------------
# 4. 目标卷空间检查（容器要占 SizeGB，留 2GB 余量）
# ---------------------------------------------------------------------------
try {
    $drv = Get-PSDrive -Name $Letter -ErrorAction Stop
    $freeGB = [math]::Round($drv.Free / 1GB, 1)
    Say ('[..] ' + $Letter + ': 可用 ' + $freeGB + ' GB')
    if ($freeGB -lt ($SizeGB + 2)) {
        Fail ('可用空间不足（需 ' + ($SizeGB + 2) + ' GB 含余量）') 7
    }
    Say '[ok] 空间足够'
} catch {
    Fail ('读取 ' + $Letter + ': 空间失败：' + $_.Exception.Message) 8
}

# 已存在的容器文件提示（不删除，只报告）
$container = $Letter + ':\RAMBoostL2c.bin'
if (Test-Path -LiteralPath $container) {
    $ci = Get-Item -LiteralPath $container
    Say ('[!!] 容器已存在：' + $container + '  ' + [math]::Round($ci.Length / 1GB, 2) + ' GB  mtime=' + $ci.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
    Say '     这不一定是问题——上一次引导留下的容器会被复用。'
} else {
    Say ('[..] 容器尚不存在，驱动打开时会创建 ' + $container)
}

Say ''

# ---------------------------------------------------------------------------
# 5. 启用前状态
# ---------------------------------------------------------------------------
Say '--- 启用前：l2 status ---'
& $ctl l2 status 2>&1 | ForEach-Object { Say ([string]$_) }
$rcBefore = $LASTEXITCODE
Say ('[..] exit=' + $rcBefore)
Say ''

# ---------------------------------------------------------------------------
# 6. 执行启用
# ---------------------------------------------------------------------------
Say '--- 执行：l2 enable ' + $Letter + ': ' + $SizeGB + ' ---'
& $ctl l2 enable ($Letter + ':') ([string]$SizeGB) 2>&1 | ForEach-Object { Say ([string]$_) }
$rcEnable = $LASTEXITCODE
Say ('[..] exit=' + $rcEnable)
Say ''

# ---------------------------------------------------------------------------
# 7. 启用后状态（这是判据来源）
# ---------------------------------------------------------------------------
Start-Sleep -Milliseconds 800
Say '--- 启用后：l2 status ---'
& $ctl l2 status 2>&1 | ForEach-Object { Say ([string]$_) }
$rcAfter = $LASTEXITCODE
Say ('[..] exit=' + $rcAfter)
Say ''

# ---------------------------------------------------------------------------
# 8. 注册表复核（独立于控制器自述）
# ---------------------------------------------------------------------------
Say '--- 注册表 Features ---'
$fk = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features'
try {
    $fp = Get-ItemProperty -LiteralPath $fk -ErrorAction Stop
    foreach ($n in @('Flags', 'L2Letter', 'L2SizeGB')) {
        $v = $fp.$n
        if ($null -eq $v) { Say ('  ' + $n + ' = (unset)') }
        else { Say ('  ' + $n + ' = ' + $v) }
    }
    if ($null -ne $fp.Flags) {
        $bit = ([int]$fp.Flags) -band 4
        Say ('  Flags & RB2F_L2(0x4) = ' + $bit + $(if ($bit -ne 0) { '  -> L2 位已置' } else { '  -> L2 位未置' }))
    }
} catch {
    Say ('  读取 Features 失败：' + $_.Exception.Message)
}
Say ''

# ---------------------------------------------------------------------------
# 9. 结论
# ---------------------------------------------------------------------------
Say '============================================================================'
if ($rcEnable -eq 0) {
    Say ' 结论：l2 enable 命令已发出（exit 0）。'
    Say ' 判据：上面「启用后 l2 status」里 flagsL2=on、L2 state 应为 opened；'
    Say '       若显示 refused（括号里给原因），说明被门禁挡下，L2 实际未启用。'
} else {
    Say (' 结论：l2 enable 返回非零（exit=' + $rcEnable + '），L2 很可能未启用。')
    Say '       上面输出里能看到具体原因。'
}
Say ''
Say ' 接下来：在 D: 上做读写以激励命中/回填，然后观察是否 BSOD。'
Say ' 关闭：rbfeatctl l2 disable（提权）'
Say (' 日志：' + $logFile)
Say '============================================================================'

if ($rcEnable -ne 0) { exit 1 }
exit 0
