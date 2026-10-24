# qa_smoke_wsconc.ps1 - [FIX #2] test_wsconc 冒烟矩阵驱动（6 组合 x 1 轮，退出码聚合）
#
# 背景：round5 备案"smoke 5-6 组合宣称不可复现"——smoke_runs.log/smoke2.log 在仓、
# 驱动脚本缺失。本脚本固化为可复现入口（与 qa_sweep_wsconc.ps1 同构，轻量版）。
#
# 用法：powershell -ExecutionPolicy Bypass -File src\scripts\qa_smoke_wsconc.ps1
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

$combos = @(
    @{ n='default';     a=@() },
    @{ n='p21';         a=@('p21') },
    @{ n='mirrorall';   a=@('mirrorall') },
    @{ n='overlap-p21'; a=@('overlap','p21') },
    @{ n='slowwb-p21';  a=@('slowwb','p21') },
    @{ n='p21-bigwrite';a=@('p21','bigwrite') }
)

$fails = 0
foreach ($c in $combos) {
    $out = & $exe @($c.a) 2>&1
    $rc = $LASTEXITCODE
    if ($rc -ne 0) {
        $fails++
        # [FIX #9] 格式串必须先算后传：Write-Host "..." -f $x 的 -f 会被解析成
        # -ForegroundColor 的缩写前缀（组合名 default 当颜色值转换即崩，实测）。
        Write-Host ("[{0,-12}] exit={1}  FAIL" -f $c.n, $rc) -ForegroundColor Red
        ($out -split "`n" | Select-Object -Last 6) | ForEach-Object { Write-Host "    $_" }
    } else {
        Write-Host ("[{0,-12}] exit=0  ok" -f $c.n) -ForegroundColor Green
    }
}
Write-Host "`nSMOKE_RESULT: fails=$fails"
if ($fails -gt 0) { Write-Host "qa_smoke: FAIL" -ForegroundColor Red; exit 1 }
Write-Host "qa_smoke: PASS" -ForegroundColor Green
exit 0
