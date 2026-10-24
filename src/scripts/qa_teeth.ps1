# qa_teeth.ps1 - [FIX #2/#9] WS-RACE-2 变异测试驱动（KILLED/SURVIVED 语义 + 退出码门禁）
#
# 背景：round5 备案"变异测试零区分力"——变异体 = WS-RACE-2 修复整体回退
# （build\qa4\cache_mut.c 两处 neutered：stage_revalidate 恒过 + wb_drain 空转），
# 旧日志 26/26 全记 PASS（判定语义失真）。本脚本把判定改成 KILLED/SURVIVED：
#   - gate 系（test_cache 用例11 计时门控牙齿）：变异体必须被杀（退出码非 0）
#     ——否则说明测试无牙口，FAIL（fail-closed，立即退出）；
#   - expect-survive 系（test_wsconc 全模式）：结构性等价，标注
#     SURVIVED-EXPECTED，不计门禁；若意外被杀只提示重分类（多牙齿是好事）。
# 双侧同参数、同轮数必须都跑：变异体被杀 + 修复侧 PASS = 牙口来自修复而非构造。
#
# 【分类依据：2026-10-21 重跑实证 + wsrace-fix-report §9.3】
# wsconc 全模式杀不死该变异体（本脚本实测 rewrite-slowwb 6/6、rewrite-p21-
# slowwb 3/3、rewrite-plain 6/6 全部存活；QA 独立矩阵 mirrorall-slowwb 0/10、
# p21-slowwb 0/6、mirrorall 0/10 同结论，忠实口径自然复现率 ~0-4%）。三重
# 结构原因（顺序单写语义不构造"旧脏版本"前置条件）：
#   (a) 数据确定性——旧 stage 与新镜像数值相同（overlap f(off)），损坏价值不可见；
#   (b) slowwb 放大 [stage,writeback] 时长、不提高"回写器恰在两笔连续写的
#       absorb 之间 stage 到边界子块"的 µs 级时机出现概率；
#   (c) 每子块基本只被写一次，无旧版可被 stage（边界子块除外）。
# 门禁牙齿唯一确定：test_cache 用例11——迟到旧数据落于新镜像之后，确定性
# 构造（计时门控把在途回写钉在镜像发布点），变异体实测 [FAIL] 盘面 got=1
# exp=9、修复侧 PASS。wsconc 系保留作回归净度（镜像/记账/链表全净），不作
# 区分力证据——"-2 路径修复有效"的结论口径 = 用例11 牙齿 + 设计级论证，
# 见 wsrace-fix-report §9.3/§十 Known Issues。
#
# 用法：powershell -ExecutionPolicy Bypass -File src\scripts\qa_teeth.ps1 [-Rounds N]
param([int]$Rounds = 6)

$ErrorActionPreference = 'Stop'
# [FIX #9 2026-10-21] 上两级必须嵌套：Split-Path -Parent 连写两次的写法在
# PS 5.1 报 ParameterAlreadyBound（本脚本首发实测崩于此行），嵌套写法等价。
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$core = Join-Path $root 'src\core'
$qa4  = Join-Path $core 'build\qa4'
New-Item -ItemType Directory -Force -Path $qa4 | Out-Null

# ── 构建双侧（cl 退出码判定 + 链接前删陈旧 exe；QA5-1 纪律）──
# [FIX #9 2026-10-21] 弃 cmd /c 包装改直接调用 cl：cmd /c 把含中文的绝对路径
# 按 OEM 代码页搅乱（实测 陈 -> 乱码），且原命令行 /Fobuild\ 与 /Fo<绝对路径>
# 双写触发 cl D9025/D8036。直接调用 + 相对 obj 目录，与 core\_build_all.ps1
# 同构（该模式在本机中文路径下长期验证可靠）。
function Build-Side($tag, $cachec, $testsrc) {
    $exe = Join-Path $qa4 "$tag.exe"
    if (Test-Path $exe) { Remove-Item -Force $exe }
    $log = Join-Path $qa4 "build_$tag.log"
    Push-Location $core
    & cl /nologo /O2 /W3 /utf-8 /I. /Ibuild\qa4 $testsrc $cachec /Fe:$exe /Fo:build\qa4\ > $log 2>&1
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
        Write-Host "[X] build $tag FAILED (cl exit=$rc) - see $log" -ForegroundColor Red
        Get-Content $log -Tail 20
        exit 1
    }
    Write-Host "[ok] built $tag" -ForegroundColor Green
}

Write-Host "== teeth: building both sides (fixed=cache.c, mutant=cache_mut.c) =="
Build-Side 'teeth_cache_fixed' (Join-Path $core 'cache.c')            (Join-Path $core 'test_cache.c')
Build-Side 'teeth_cache_mut'   (Join-Path $qa4  'cache_mut.c')        (Join-Path $core 'test_cache.c')
Build-Side 'teeth_ws_fixed'    (Join-Path $core 'cache.c')            (Join-Path $core 'test_wsconc.c')
Build-Side 'teeth_ws_mut'      (Join-Path $qa4  'cache_mut.c')        (Join-Path $core 'test_wsconc.c')

# gate 系 = 必须杀死的确定性牙齿（唯一项：cache 用例11；3 轮求稳，计时门控）
$gate = @(
    @{ n='cache-用例11(计时门控)'; exe='teeth_cache_mut.exe'; fix='teeth_cache_fixed.exe'; a=@(); rounds=3 }
)
# expect-survive 系 = 结构性等价（wsconc 全模式；分类依据见头部 §9.3 摘要）
$expectSurvive = @(
    @{ n='wsconc-rewrite-slowwb';     a=@('rewrite','slowwb') },
    @{ n='wsconc-rewrite-p21-slowwb'; a=@('rewrite','p21','slowwb') },
    @{ n='wsconc-rewrite-plain';      a=@('rewrite') },
    @{ n='wsconc-mirrorall-slowwb';   a=@('mirrorall','slowwb') },
    @{ n='wsconc-p21-slowwb';         a=@('p21','slowwb') },
    @{ n='wsconc-mirrorall-plain';    a=@('mirrorall') }
)

$killed = 0; $ran = 0
Write-Host "`n== gate series (mutant MUST be killed; fixed side MUST pass) =="
foreach ($t in $gate) {
    $seriesKilled = 0
    for ($r = 1; $r -le $t.rounds; $r++) {
        $exe = Join-Path $qa4 $t.exe
        $out = & $exe @($t.a) 2>&1
        $rc = $LASTEXITCODE
        $ran++
        if ($rc -ne 0) { $killed++; $seriesKilled++ }
        else {
            Write-Host "  [SURVIVED] $($t.n) round $r : mutant survived (no tooth)" -ForegroundColor Red
            ($out -split "`n" | Select-Object -Last 6) | ForEach-Object { Write-Host "    $_" }
        }
    }
    $fixexe = Join-Path $qa4 $t.fix
    for ($r = 1; $r -le $t.rounds; $r++) {
        $outf = & $fixexe @($t.a) 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  [X] FIXED side FAILED on $($t.n) round $r - construction broken, not a real tooth" -ForegroundColor Red
            ($outf -split "`n" | Select-Object -Last 8) | ForEach-Object { Write-Host "    $_" }
            exit 1
        }
    }
    # [FIX #9] 如实记账：任一 gate 轮次存活即 FAIL（旧版无条件打印 killed 是说谎）
    if ($seriesKilled -ne $t.rounds) {
        Write-Host "  [X] $($t.n) : mutant survived $($t.rounds - $seriesKilled)/$($t.rounds) round(s) - test has no tooth" -ForegroundColor Red
        exit 1
    }
    Write-Host "  [ok] $($t.n) : mutant killed in all $($t.rounds) round(s); fixed side PASS" -ForegroundColor Green
}

Write-Host "`n== expected-survive series (informational; structural equivalence) =="
$survExp = 0
foreach ($t in $expectSurvive) {
    $exe = Join-Path $qa4 'teeth_ws_mut.exe'
    $out = & $exe @($t.a) 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host "  [KILLED!] $($t.n) : unexpected kill - re-classify as gate" -ForegroundColor Yellow }
    else { $survExp++; Write-Host "  [SURVIVED-expected] $($t.n)" -ForegroundColor DarkGray }
}

Write-Host "`nMUTATION_SCORE: killed=$killed/$ran (gate series), survived-expected=$survExp/$($expectSurvive.Count)"
if ($killed -ne $ran) {
    Write-Host "teeth: FAIL (mutant survived a gate round - test has no tooth)" -ForegroundColor Red
    exit 1
}
Write-Host "teeth: PASS (all gate rounds killed; fixed side clean)" -ForegroundColor Green
exit 0
