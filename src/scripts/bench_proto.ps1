#requires -Version 5.1
<#
.SYNOPSIS
    bench_proto.ps1 —— 计数器判别式对账脚本（W1.5 读写路径专项 + 内存级速度专项）。
.DESCRIPTION
    AS SSD 2.0.7316 / CDM 9.0.1 均无 CLI 静默模式——跑分动作由人执行；本脚本负责：
      attached/detached 对照 + 二遍读协议轮次提示 + ReadReq/ReadHit/ReadHitBytes
      计数器抓取（Get-RbStats，dot-source rb_common.ps1）+ 3 轮中位 + CSV 输出。
    [W1.5] 判别式（设计 §9）：
      ① 计数器对账：分数变化与 ReadReq/命中率曲线同向；>8MB 的 IO 进入缓存面后
         应出现在 ReadReq 增量中；detached 态驱动不可见 → ReadReq 增量应为 0；
      ② 二遍读协议：同文件冷/热两轮——热轮达标 + 冷轮无回归 = 门+缓存面生效；
      ③ detached 裸盘基线一轮（B）：把 4K 写改善拆成"消除负优化"vs"回写吸收"归因；
      ④ 3 轮取中位抗噪；任一轮 Verifier 新告警即该轮作废（人工判据，脚本外）。
    [内存级专项 T05] Phase 1b 矩阵（每场景 3 轮取中位，判据见 README 手册）：
      SEQ1M 读/写 × Q8T1/Q1T1（写：Q8T1 ≥ Q1T1 消除倒退为 D2 主判据）、
      RND4K 读/写 × Q32T1/Q1T1（QD1 为热轮判据：先热一轮再测，冷轮=盘物理下限）。
      计数器增量含 LostDirtyDelta（C7 记账回归：失败路径/竞态不得丢脏无账）。
    数据安全回归点（脚本末尾自动执行）：detach 后 DirtyBytes 应为 0（全量回写）；
      WbFlush/WbBytes 增量序列供人工核对 ~60s 周期落盘节奏（CSV 内）。
    错误处理约定（规格书 D.3 / run_diskspd.ps1 同款）：$ErrorActionPreference='Stop'，
    失败不静默、以非零码退出；ctl 缺失/非零退出/关键行解析失败 → fail-closed。
.PARAMETER Letter
    被测卷盘符（默认 D）。
.PARAMETER TestFile
    测试文件路径（仅用于提示文案；默认 <Letter>:\asssd_test.bin）。
.PARAMETER Rounds
    每场景轮数（默认 3，取中位）。
.PARAMETER OutDir
    CSV 输出目录（默认 <repo>\bench_out-<时间戳>）。
.PARAMETER SkipDetached
    跳过 detached 裸盘基线轮（默认执行；仅当卷无法 detach 时使用）。
.PARAMETER DryRun
    干跑：不调用 ctl、合成全零计数器快照——用于无驱动环境验证脚本机制
    （字段齐全的 CSV 仍会输出，DryRun 列标记为 yes）。
.PARAMETER Auto
    自动应答所有人工提示（分数记为空=未记录）——非交互干跑/CI 冒烟用。
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\bench_proto.ps1 -Letter D
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\bench_proto.ps1 -DryRun -Auto
#>
[CmdletBinding()]
param(
    [char]$Letter = 'D',
    [string]$TestFile = '',
    [int]$Rounds = 3,
    [string]$OutDir = '',
    [switch]$SkipDetached,
    [switch]$DryRun,
    [switch]$Auto
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rb_common.ps1')
# [FIX S-DRYRUN] DryRun 不调用 ctl、不碰驱动（计数器为合成零值），无需管理员；
# 非 DryRun 的 attach/detach/status 走 Assert-Admin（fail-closed 不变）。
if (-not $DryRun) { Assert-Admin }

if ($Rounds -lt 1) { throw '-Rounds 必须 >= 1（3 轮取中位是协议口径）。' }
if ([string]::IsNullOrWhiteSpace($TestFile)) { $TestFile = "$Letter`:\asssd_test.bin" }
$LetterStr = [string]$Letter

$ctl = Join-Path $ProjectRoot 'ctl\ramboostctl.exe'
if (-not $DryRun -and -not (Test-Path -LiteralPath $ctl)) {
    throw "未找到 $ctl（先编译 ctl；或先用 -DryRun 验证脚本机制）。"
}

$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = Join-Path $ProjectRoot "bench_out-$stamp" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$rows = New-Object System.Collections.Generic.List[object]

# ---- 原生调用统一收口 ----
# 局部 EAP=Continue（Sign-Driver/Get-RbStats 同款模式）：stderr 合流不触发 Stop；
# 成败由 $LASTEXITCODE 显式判定——fail-closed 不依赖 EAP 副作用。
function Invoke-Ctl([string[]]$CtlArgs) {
    $ErrorActionPreference = 'Continue'
    $out = & $ctl @CtlArgs 2>&1
    $code = $LASTEXITCODE
    return @{ Out = ($out | Out-String); Code = $code }
}

# ---- 计数器快照（干跑=合成全零）----
function Get-Snap([string]$Tag) {
    if ($DryRun) {
        Write-Host "  [snap:$Tag] (dry-run 合成全零)"
        return [pscustomobject]@{
            ReadReq = [uint64]0; ReadHitPct = [double]0; ReadHit = [double]0
            ReadHitBytesPct = [double]0; WriteReq = [uint64]0; WbFlush = [uint64]0
            DirtyMB = [double]0; DirtyBytes = [uint64]0; WbBytes = [uint64]0
            LostDirtyBytes = [uint64]0
        }
    }
    return (Get-RbStats -CtlPath $ctl -Raw)
}

function Wait-Operator([string]$Msg) {
    Write-Host "`n===== $Msg =====" -ForegroundColor Cyan
    if ($Auto) { Write-Host '  [auto] 跳过人工等待'; return }
    Read-Host '  该轮跑完后按回车继续'
}

function Read-Score([string]$What) {
    if ($Auto) { Write-Host "  [auto] $What：未记录"; return $null }
    $v = Read-Host "  请输入 $What（MB/s，Benchmark 界面读数；留空=未记录）"
    if ([string]::IsNullOrWhiteSpace($v)) { return $null }
    $d = 0.0
    if ([double]::TryParse($v.Trim(), [ref]$d)) { return $d }
    Write-Host '  [警告] 无法解析为数字，按未记录处理。' -ForegroundColor Yellow
    return $null
}

function Add-Row([string]$Mode, [string]$Scenario, [int]$Round, [string]$Pass, $Score, $Before, $After) {
    $readReqDelta = [int64]($After.ReadReq - $Before.ReadReq)
    $scoreText = if ($null -eq $Score) { '' } else { $Score.ToString('F2') }
    $rows.Add([pscustomobject]@{
        Mode             = $Mode
        Scenario         = $Scenario
        Round            = $Round
        Pass             = $Pass
        DryRun           = if ($DryRun) { 'yes' } else { 'no' }
        ScoreMBps        = $scoreText
        ReadReq          = $After.ReadReq
        ReadReqDelta     = $readReqDelta
        ReadHitPct       = $After.ReadHitPct
        ReadHitBytesPct  = $After.ReadHitBytesPct
        ReadHitDelta     = [math]::Round($After.ReadHit - $Before.ReadHit, 2)
        WriteReqDelta    = [int64]($After.WriteReq - $Before.WriteReq)
        WbFlushDelta     = [int64]($After.WbFlush - $Before.WbFlush)
        WbBytesDelta     = [int64]($After.WbBytes - $Before.WbBytes)
        LostDirtyDelta   = [int64]($After.LostDirtyBytes - $Before.LostDirtyBytes)
        DirtyBytesAfter  = $After.DirtyBytes
        Timestamp        = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
    })
}

function Get-Median($Values) {
    $v = @($Values | Where-Object { $_ -ne $null -and $_ -gt 0 } | Sort-Object)
    if ($v.Count -eq 0) { return $null }
    $n = $v.Count
    if ($n % 2 -eq 1) { return [double]$v[[int](($n - 1) / 2)] }
    return ([double]$v[$n / 2 - 1] + [double]$v[$n / 2]) / 2.0
}

# [T05] 内存级矩阵场景表：Key=CSV/摘要标识，Prompt=人工提示，Pass=冷/热轮次语义。
# QD1 场景按二遍读协议：先热一轮（重建/重读测试文件）再测，热轮为内存级判据轮。
$Scenarios = [ordered]@{
    'seq-r-q8'  = @{ Prompt = 'AS SSD「Seq」读（QD8）';                    Pass = 'hot'  }
    'seq-w-q8'  = @{ Prompt = 'AS SSD「Seq」写（QD8，回写开）';             Pass = 'hot'  }
    'seq-r-q1'  = @{ Prompt = 'AS SSD「Seq」读（QD1）';                    Pass = 'hot'  }
    'seq-w-q1'  = @{ Prompt = 'AS SSD「Seq」写（QD1，回写开）';             Pass = 'hot'  }
    '4k-r-q32'  = @{ Prompt = 'AS SSD「4K」读（QD32）';                    Pass = 'hot'  }
    '4k-w-q32'  = @{ Prompt = 'AS SSD「4K」写（QD32，回写开）';             Pass = 'hot'  }
    '4k-r-q1'   = @{ Prompt = 'AS SSD「4K」读（QD1）【先热一轮：同文件第二遍】'; Pass = 'hot' }
    '4k-w-q1'   = @{ Prompt = 'AS SSD「4K」写（QD1，回写开）【先热一轮】';   Pass = 'hot'  }
}

Write-Host '== bench_proto (计数器判别式 + 内存级矩阵) ==' -ForegroundColor Cyan
Write-Host "  卷=$LetterStr  测试文件=$TestFile  轮数=$Rounds  输出=$OutDir"
if ($DryRun) { Write-Host '  [DRY-RUN] 不调用 ctl，计数器为合成零值（机制验证用）' -ForegroundColor Yellow }

# ================= Phase 0: detached 裸盘基线（B 轮）=================
if (-not $SkipDetached) {
    Write-Host "`n[Phase 0] detached 基线：分离卷 $LetterStr（裸盘对照）..."
    if ($DryRun) {
        Write-Host '  [dry-run] 跳过 detach'
    } else {
        $r = Invoke-Ctl @('detach', $LetterStr)
        if ($r.Code -ne 0) {
            Write-Host "  [警告] detach 返回 $($r.Code)（卷可能本就未 attach，继续）：$($r.Out)" -ForegroundColor Yellow
        }
    }
    $b = Get-Snap 'detached-before'
    Wait-Operator "detached 基线 · 第一遍（冷）读：对 $TestFile 运行 AS SSD「Seq」读"
    $c = Get-Snap 'detached-cold'
    Add-Row 'detached' 'seq-r' 0 'cold' (Read-Score 'detached 冷读 Seq 读 MB/s') $b $c
    Wait-Operator 'detached 基线 · 第二遍（热）读：同文件再跑一次 AS SSD「Seq」读'
    $h = Get-Snap 'detached-hot'
    Add-Row 'detached' 'seq-r' 0 'hot' (Read-Score 'detached 热读 Seq 读 MB/s') $c $h
}

# ================= Phase 1: attached Seq 读对照（W1.5 既有口径）=================
Write-Host "`n[Phase 1] attached 对照：挂载卷 $LetterStr 进入缓存..."
if ($DryRun) {
    Write-Host '  [dry-run] 跳过 attach'
} else {
    $r = Invoke-Ctl @('attach', $LetterStr)
    if ($r.Code -ne 0) { throw "attach $LetterStr 失败（exit=$($r.Code)）：$($r.Out)" }
}
for ($i = 1; $i -le $Rounds; $i++) {
    $b = Get-Snap "attached-r$i-before"
    Wait-Operator "第 $i 轮 · 第一遍（冷）读：重建测试文件（删除后让 AS SSD 重写）再跑 AS SSD「Seq」读"
    $c = Get-Snap "attached-r$i-cold"
    Add-Row 'attached' 'seq-r' $i 'cold' (Read-Score "第 $i 轮 冷读 Seq 读 MB/s") $b $c
    Wait-Operator "第 $i 轮 · 第二遍（热）读：同文件再跑一次 AS SSD「Seq」读（第二遍应命中缓存）"
    $h = Get-Snap "attached-r$i-hot"
    Add-Row 'attached' 'seq-r' $i 'hot' (Read-Score "第 $i 轮 热读 Seq 读 MB/s") $c $h
}

# ================= Phase 1b: 内存级矩阵（T05）=================
# 每场景 3 轮取中位；QD1 场景的热轮判据要求"先热一轮"（提示文案已含）。
# 计数器增量（含 LostDirtyDelta）逐轮入 CSV——C7 记账回归：任何场景
# lost_dirty 增量应为 0（除非人为注入盘错，属预期失败路径）。
Write-Host "`n[Phase 1b] 内存级矩阵：$($Scenarios.Count) 个场景 × $Rounds 轮（人工跑分）..."
foreach ($key in $Scenarios.Keys) {
    $sc = $Scenarios[$key]
    for ($i = 1; $i -le $Rounds; $i++) {
        $b = Get-Snap "mx-$key-r$i-before"
        Wait-Operator ("矩阵 [{0}] 第 {1} 轮：{2}" -f $key, $i, $sc.Prompt)
        $a = Get-Snap "mx-$key-r$i-after"
        Add-Row 'matrix' $key $i $sc.Pass (Read-Score ("[{0}] 第 {1} 轮 MB/s" -f $key, $i)) $b $a
    }
}

# ================= Phase 2: 数据安全回归点 =================
Write-Host "`n[Phase 2] 数据安全回归：detach 前应已完成全量回写（DirtyBytes=0）..."
if ($DryRun) {
    Write-Host '  [dry-run] 跳过 detach/DirtyBytes 校验'
} else {
    $r = Invoke-Ctl @('detach', $LetterStr)
    if ($r.Code -ne 0) { Write-Host "  [警告] detach 返回 $($r.Code)：$($r.Out)" -ForegroundColor Yellow }
    $after = Get-RbStats -CtlPath $ctl -Raw
    if ($after.DirtyBytes -ne 0) {
        Write-Host "  [警告] detach 后 DirtyBytes=$($after.DirtyBytes)（期望 0：全量回写未完成？）" -ForegroundColor Yellow
    } else {
        Write-Host '  [ok] detach 后 DirtyBytes=0（全量回写完成，fix-report-2026-10-17 既有验收项）' -ForegroundColor Green
    }
    if ($after.LostDirtyBytes -ne 0) {
        Write-Host "  [警告] LostDirtyBytes=$($after.LostDirtyBytes)（C7 记账非零：检查是否有盘错/失败路径被触发）" -ForegroundColor Yellow
    } else {
        Write-Host '  [ok] LostDirtyBytes=0（C7 记账干净：无非预期丢弃）' -ForegroundColor Green
    }
}

# ================= Phase 3: 中位 + CSV + 判别式摘要 =================
$detCold = @($rows | Where-Object { $_.Mode -eq 'detached' -and $_.Pass -eq 'cold' -and $_.ScoreMBps -ne '' } | ForEach-Object { [double]$_.ScoreMBps })
$detHot  = @($rows | Where-Object { $_.Mode -eq 'detached' -and $_.Pass -eq 'hot'  -and $_.ScoreMBps -ne '' } | ForEach-Object { [double]$_.ScoreMBps })
$attCold = @($rows | Where-Object { $_.Mode -eq 'attached'  -and $_.Pass -eq 'cold' -and $_.ScoreMBps -ne '' } | ForEach-Object { [double]$_.ScoreMBps })
$attHot  = @($rows | Where-Object { $_.Mode -eq 'attached'  -and $_.Pass -eq 'hot'  -and $_.ScoreMBps -ne '' } | ForEach-Object { [double]$_.ScoreMBps })

$csv = Join-Path $OutDir "bench_proto-$stamp.csv"
$rows | Export-Csv -LiteralPath $csv -NoTypeInformation -Encoding UTF8

Write-Host "`n===== 中位摘要（每场景 3 轮取中位；detached 为单轮 B）=====" -ForegroundColor Cyan
function Show-Med([string]$Label, $Vals) {
    $m = Get-Median $Vals
    if ($null -eq $m) { Write-Host ("  {0,-28} : 无有效分数（未记录）" -f $Label); return }
    Write-Host ("  {0,-28} : {1:F2} MB/s（n={2}）" -f $Label, $m, $Vals.Count)
}
Show-Med 'detached 冷读' $detCold
Show-Med 'detached 热读' $detHot
Show-Med 'attached 冷读（3 轮中位）' $attCold
Show-Med 'attached 热读（3 轮中位）' $attHot
foreach ($key in $Scenarios.Keys) {
    $vals = @($rows | Where-Object { $_.Mode -eq 'matrix' -and $_.Scenario -eq $key -and $_.ScoreMBps -ne '' } | ForEach-Object { [double]$_.ScoreMBps })
    Show-Med ("矩阵 $key") $vals
}

Write-Host "`n===== 判别式辅助（数据供 QA 对账，最终判定按 README 手册判据表）=====" -ForegroundColor Cyan
$detRows = @($rows | Where-Object { $_.Mode -eq 'detached' })
$attRows = @($rows | Where-Object { $_.Mode -eq 'attached' })
$detReq = ($detRows | Measure-Object -Property ReadReqDelta -Sum).Sum
$attReq = ($attRows | Measure-Object -Property ReadReqDelta -Sum).Sum
Write-Host ("  门生效证据：ReadReq 增量 detached={0} / attached={1}（>8MB IO 进入缓存面才计数；detached 驱动不可见应为 0）" -f $detReq, $attReq)
$hotHit = ($attRows | Where-Object { $_.Pass -eq 'hot' } | Measure-Object -Property ReadHitPct -Average).Average
$coldHit = ($attRows | Where-Object { $_.Pass -eq 'cold' } | Measure-Object -Property ReadHitPct -Average).Average
if ($null -ne $hotHit -and $null -ne $coldHit) {
    Write-Host ("  命中曲线：attached 冷轮均命中 {0:F2}% → 热轮均命中 {1:F2}%（与分数同向=缓存命中所致）" -f $coldHit, $hotHit)
}
# [D2 主判据] SEQ 写 Q8T1 vs Q1T1：消除倒退（Q8T1 ≥ Q1T1）
$w8 = Get-Median @($rows | Where-Object { $_.Mode -eq 'matrix' -and $_.Scenario -eq 'seq-w-q8' -and $_.ScoreMBps -ne '' } | ForEach-Object { [double]$_.ScoreMBps })
$w1 = Get-Median @($rows | Where-Object { $_.Mode -eq 'matrix' -and $_.Scenario -eq 'seq-w-q1' -and $_.ScoreMBps -ne '' } | ForEach-Object { [double]$_.ScoreMBps })
if ($null -ne $w8 -and $null -ne $w1) {
    $verdict = if ($w8 -ge $w1) { 'PASS（倒退已消除）' } else { 'FAIL（仍倒退）' }
    Write-Host ("  [D2 主判据] SEQ 写 Q8T1={0:F2} vs Q1T1={1:F2} → {2}" -f $w8, $w1, $verdict)
} else {
    Write-Host '  [D2 主判据] SEQ 写 Q8T1/Q1T1 分数未记录（矩阵未跑或 Auto 模式）'
}
$mxLost = ($rows | Where-Object { $_.Mode -eq 'matrix' } | Measure-Object -Property LostDirtyDelta -Sum).Sum
Write-Host ("  C7 记账：矩阵 LostDirtyDelta 合计={0}（应为 0；非 0 说明有失败路径/竞态丢脏无账）" -f $mxLost)
Write-Host '  落盘节奏：见 CSV WbFlushDelta/WbBytesDelta 序列（~60s 脉动为正常，人工核对）'
Write-Host "  CSV: $csv"
Write-Host '  提示：任一轮 Driver Verifier 新告警 → 该轮作废重跑（人工判据）。'
Write-Host '== bench_proto done =='
