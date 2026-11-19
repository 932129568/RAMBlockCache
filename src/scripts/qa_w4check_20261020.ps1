# /W4 clean gate for the cache engine TUs.
#
# [G20 2026-11-11] 两处静默失效已修（审计 finding P0-1），变异已验证：
#  ① cl.exe 不存在时 `& $cl ...` 抛的是"不会被识别为 cmdlet"的**命令解析错误**，
#     `$LASTEXITCODE` 保持 `$null` 而非非零；`$rcAll += $null` 仍为 0 →
#     6 个 TU 全打印 "rc= warnings=0"，最终 `exit (1 - [int](0 -eq 0))` = **0**。
#     即 /W4 门禁在工具链搬家后**完全静默变绿**（实跑复现：EXIT=0）。
#     现在：① 编译前 `Test-Path $cl`，缺失即 fail-closed；② 每个 TU 显式
#     校验 `$null -ne $LASTEXITCODE`，为 null 视作 rc=127；③ 末尾若产物
#     一个都没生成，也判失败。
#  ② `$root` 原写死为 `C:\Users\陈\Desktop\...`，脚本换目录后编译的是**桌面那份**
#     源码而不是脚本所在树的源码（审计 P2-5）。现在从 `$PSScriptRoot` 推导。
#
# 说明：本脚本**不在** `-Target gates` 的 17 项内（需 MSVC，仅手动运行）。
$ErrorActionPreference = 'Stop'

# ── 仓库根由脚本位置推导：<repo>/src/scripts/qa_w4check_*.ps1 → <repo> ──
if (-not $PSScriptRoot) { throw "PSScriptRoot unavailable; run via -File, not dot-source" }
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
Write-Output "repo root: $root"

# ── 工具链：优先环境变量，其次已知安装位置；都找不到则 fail-closed ──
# 注意 @(...)：Where-Object 命中 1 个时返回的是标量，`$x[0]` 会取到字符串
# 首字符（'C:\Program1\...'[0] = 'C'），随后拼出 'C\bin\Hostx64\x64\cl.exe'。
# 这是 PowerShell 的经典坑，必须强制成数组。
$msvcCandidates = @(@(
    $env:RBC_MSVC_DIR,
    'C:\Program1\VC\Tools\MSVC\14.44.35207'
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
if ($msvcCandidates.Count -eq 0) {
    throw "MSVC toolchain not found (set RBC_MSVC_DIR=<VC\Tools\MSVC\<ver>>). /W4 gate cannot run blind."
}
$msvc = $msvcCandidates[0]
$kit  = if ($env:RBC_WINSDK_DIR) { $env:RBC_WINSDK_DIR } else { 'C:\Program Files (x86)\Windows Kits\10' }
if (-not (Test-Path -LiteralPath $kit)) { throw "Windows SDK not found: $kit" }

$cl = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
# 关键防线 1：cl.exe 必须真实存在，否则下面每个 TU 的 rc 都会是 $null
if (-not (Test-Path -LiteralPath $cl)) {
    throw "cl.exe missing: $cl -- refusing to report /W4 clean without a compiler"
}
Write-Output "cl.exe: $cl"

$sdkVer = @(Get-ChildItem -LiteralPath (Join-Path $kit 'Include') -Directory |
            Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' } |
            Sort-Object Name -Descending | Select-Object -First 1)[0].Name
if (-not $sdkVer) { throw "no Windows SDK Include\10.0.*.* under $kit" }
Write-Output "Windows SDK: $sdkVer"

$inc = @(
    "/I$root\src\core",
    "/I$msvc\include",
    "/I$kit\Include\$sdkVer\ucrt",
    "/I$kit\Include\$sdkVer\um",
    "/I$kit\Include\$sdkVer\shared",
    "/I$root\src\common",
    "/I$root\tests\core"
)

# [142] pthread shim moved src/core/usertest -> tests/core ([140 B3]); with the
# old path every cache TU died at C1083 (pthread.h) yet the script still printed
# "warnings=0" per file - a silently dead /W4 gate.
# [142] engine split into six TUs; W4-clean applies to every cache TU.
$files = @(
    "$root\src\core\cache.c",       "$root\src\core\cache_stats.c",
    "$root\src\core\cache_read.c",  "$root\src\core\cache_write.c",
    "$root\src\core\cache_flush.c", "$root\src\core\cache_evict.c"
)

$old = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$rcAll = 0; $wAll = 0; $objsBuilt = 0; $nullRc = 0
foreach ($f in $files) {
    $leaf = [IO.Path]::GetFileNameWithoutExtension($f)
    $obj  = Join-Path $env:TEMP "qa_cache_w4_$leaf.obj"
    $logf = Join-Path $env:TEMP "qa_cache_w4_$leaf.log"
    & $cl /nologo /W4 /utf-8 /MT $inc /c $f /Fo:$obj 2>&1 | Out-File -FilePath $logf -Encoding utf8
    $raw = $LASTEXITCODE
    # 关键防线 2：命令解析失败（找不到 exe）不会写 $LASTEXITCODE，它是 $null。
    # 若直接 `$rc = $LASTEXITCODE; $rcAll += $rc`，$null 加进去等于没加 → 假绿。
    if ($null -eq $raw) {
        $rc = 127                      # 与 POSIX "command not found" 同义
        $nullRc++
    } else {
        $rc = $raw
    }
    $lines = @(Get-Content $logf)
    $w = @($lines | Where-Object { $_ -match 'warning' }).Count
    if (Test-Path -LiteralPath $obj) { $objsBuilt++ }
    Write-Output ("{0} /W4 compile rc={1} warnings={2}" -f (Split-Path $f -Leaf), $rc, $w)
    if ($rc -ne 0) { $lines | Select-Object -First 10 }
    $rcAll += $rc; $wAll += $w
}
$ErrorActionPreference = $old

Write-Output "cache TU set /W4 total rc=$rcAll warnings=$wAll objects=$objsBuilt/$($files.Count)"

# 关键防线 3：即使 rc 汇总因某种原因仍是 0，产物数量不对也必须失败。
$fail = $false
if ($nullRc -gt 0) {
    Write-Output "[FAIL] $nullRc TU(s) produced no exit code (compiler never ran)"
    $fail = $true
}
if ($rcAll -ne 0) {
    Write-Output "[FAIL] nonzero compiler exit code (rc=$rcAll)"
    $fail = $true
}
if ($objsBuilt -lt $files.Count) {
    Write-Output "[FAIL] only $objsBuilt/$($files.Count) .obj produced"
    $fail = $true
}
if ($fail) { exit 1 }
Write-Output 'PASS: all cache TUs /W4 clean'
exit 0
