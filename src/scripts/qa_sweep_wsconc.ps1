# qa_sweep_wsconc.ps1 - [FIX #2] test_wsconc 模式矩阵驱动（退出码聚合，替代不入仓的手跑矩阵）
#
# 背景：round5 备案"SWEEP 60/60 宣称不可复现"——日志（build/qa4_final.log）在仓、
# 驱动脚本缺失。本脚本把模式矩阵固化为可复现入口：10 模式 x N 轮，逐轮取
# 退出码，fails>0 即 exit 1（与 build_all/run_all 的 QA5-1 退出码纪律同构）。
# 模式表在脚本顶部显式列出——日志与实现永不漂移。
#
# 用法：powershell -ExecutionPolicy Bypass -File src\scripts\qa_sweep_wsconc.ps1 [-Rounds N]
param([int]$Rounds = 6)

$ErrorActionPreference = 'Stop'
# [FIX #9 2026-10-21] 上两级必须嵌套：Split-Path -Parent 连写两次的写法在
# PS 5.1 报 ParameterAlreadyBound（qa_teeth 首发实测崩于同行），嵌套写法等价。
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$core = Join-Path $root 'src\core'
$exe  = Join-Path $core 'build\test_wsconc.exe'
if (-not (Test-Path $exe)) {
    Write-Host "[X] $exe not found - run src\core\build_all.bat first" -ForegroundColor Red
    exit 1
}

# [FIX R9-13] overlap-p21 每模式独立轮数（r 字段，缺省 = -Rounds）：该组合是
# "失效销毁+同起点重建+stage 快照过期"竞态的唯一密集爆点（p21 下每笔多区间写
# 都走 -2→失效→销毁→重建），且单轮仅 ~0.09s；100 轮 ≈ 9s，对 ~0.8%/轮的静默
# 写坏回归率有实质捕获概率（test_wsconc.c 的 wb_fn oracle 会在第一笔污染回写
# 即硬失败并打印现场）。
$modes = @(
    @{ n='default';        a=@() },
    @{ n='noflush';        a=@('noflush') },
    @{ n='bigcache';       a=@('bigcache') },
    @{ n='mirrorall';      a=@('mirrorall') },
    @{ n='bigwrite';       a=@('bigwrite') },
    @{ n='p21';            a=@('p21') },
    @{ n='p21-bigwrite';   a=@('p21','bigwrite') },
    @{ n='overlap';        a=@('overlap') },
    @{ n='overlap-p21';    a=@('overlap','p21'); r=100 },
    @{ n='slowwb-p21';     a=@('slowwb','p21') }
)

$logDir = Join-Path $core 'build'
$log = Join-Path $logDir ("qa_sweep_{0:yyyyMMdd_HHmmss}.log" -f (Get-Date))
$fails = 0; $runs = 0
# [FIX #9 2026-10-21] 原此处有一行 Start-Process --help "预热"——两个缺陷：
# ① test_wsconc 无 help 模式、未知参数忽略，该行白跑一轮完整测试，零验证价值；
# ② 本会话环境同时存在 HTTPS_PROXY/https_proxy 大小写重复键，Start-Process
# 构建环境块抛 ArgumentException 直接终断脚本（qa_run_core_20261020.ps1 头部
# 注释记录过同坑）。主循环首轮即是发射检查（& 直接调用 + 退出码捕获），移除。

foreach ($m in $modes) {
    # [FIX R9-14] hashtable 原生 ContainsKey——$m.PSObject.Properties.Name 对
    # hashtable 的键适配不可靠（实测 -contains 'r' 恒假，100 轮覆盖静默失效）。
    $mrounds = $Rounds
    if ($m.ContainsKey('r')) { $mrounds = [int]$m.r }
    for ($r = 1; $r -le $mrounds; $r++) {
        $out = & $exe @($m.a) 2>&1
        $rc = $LASTEXITCODE
        $runs++
        $line = "[{0,-14} round {1,2}] exit={2}" -f $m.n, $r, $rc
        if ($rc -ne 0) {
            $fails++
            Write-Host "$line  FAIL" -ForegroundColor Red
            ($out -split "`n" | Select-Object -Last 8) | ForEach-Object { Write-Host "    $_" }
        } else {
            Write-Host "$line  ok" -ForegroundColor Green
        }
        Add-Content -Path $log -Value $line
        if ($rc -ne 0) { ($out -split "`n") | ForEach-Object { Add-Content -Path $log -Value "    $_" } }
    }
}

Write-Host "`nSWEEP_RESULT: runs=$runs fails=$fails (log: $log)"
if ($fails -gt 0) { Write-Host "qa_sweep: FAIL" -ForegroundColor Red; exit 1 }
Write-Host "qa_sweep: PASS ($runs runs)" -ForegroundColor Green
exit 0
