[CmdletBinding()]
param(
    [ValidateSet("Baseline", "Green", "W2", "Gates")]
    [string]$Suite = "Baseline",
    [string]$DotNet = "C:\Users\陈\.dotnet\dotnet.exe",
    [string]$OutputRoot = ""
)

$ErrorActionPreference = "Continue"
$srcRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $srcRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $env:LOCALAPPDATA "Temp\rbc-recovery-qa\W0-W1\$Suite"
}
New-Item -ItemType Directory -Force $OutputRoot | Out-Null
$script:ExpectedFailures = 0
$script:UnexpectedFailures = 0

function Invoke-Native {
    param(
        [string]$Name,
        [string]$FilePath,
        [string[]]$Arguments,
        [int]$ExpectedExit = 0
    )
    $stdout = Join-Path $OutputRoot "$Name.stdout.txt"
    $stderr = Join-Path $OutputRoot "$Name.stderr.txt"
    $command = Join-Path $OutputRoot "$Name.command.txt"
    $quoted = @($FilePath) + $Arguments | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }
    [System.IO.File]::WriteAllLines($command, $quoted, [System.Text.Encoding]::UTF8)

    # [G18 2026-11-11] 两处 fail-open：
    # ① `& $FilePath ...` 在文件不存在时抛的是**命令解析错误**，`$LASTEXITCODE`
    #    不会被写入，而是**继承上一条命令的退出码**。实测：上一条 exit 0 → 本步
    #    伪装成 0；上一条 exit 7 → 本步报 7（看似有诊断，其实指错方向）。
    # ② 同理 `$LASTEXITCODE` 本身可能是 $null（从未运行过任何原生命令时），
    #    `[string]$null` 写进 exit.txt 是空串，后续解析会当成"没有退出码"。
    #
    # 注意 ① 的修法**不能**一律 Test-Path：$FilePath 可能是裸命令名
    # (`powershell.exe`、`dotnet`)，由 PATH 解析，字面 Test-Path 必然为假。
    # 因此只在"看起来是路径"时（含 \ 或 / 或盘符冒号）做存在性预检。
    $looksLikePath = ($FilePath -match '[\\/]' -or $FilePath -match '^[A-Za-z]:')
    if ($looksLikePath -and -not (Test-Path -LiteralPath $FilePath)) {
        $actual = 127
        Write-Output "$Name exit=$actual MISSING-EXE $FilePath"
        [System.IO.File]::WriteAllText((Join-Path $OutputRoot "$Name.exit.txt"), [string]$actual, [System.Text.Encoding]::ASCII)
        $script:UnexpectedFailures++
        return
    }
    # 裸命令名先确认可解析；解析不到同样归一为 127。
    if (-not $looksLikePath) {
        $cmd = Get-Command $FilePath -ErrorAction SilentlyContinue
        if (-not $cmd) {
            $actual = 127
            Write-Output "$Name exit=$actual MISSING-CMD $FilePath (not on PATH)"
            [System.IO.File]::WriteAllText((Join-Path $OutputRoot "$Name.exit.txt"), [string]$actual, [System.Text.Encoding]::ASCII)
            $script:UnexpectedFailures++
            return
        }
    }
    & $FilePath @Arguments > $stdout 2> $stderr
    $actual = $LASTEXITCODE
    if ($null -eq $actual) {
        Write-Output "$Name exit=<null> (native run never set LASTEXITCODE)"
        $actual = 127
    }
    [System.IO.File]::WriteAllText((Join-Path $OutputRoot "$Name.exit.txt"), [string]$actual, [System.Text.Encoding]::ASCII)
    Write-Output "$Name exit=$actual expected=$ExpectedExit"
    if ($actual -ne $ExpectedExit) { $script:UnexpectedFailures++ }
}

if ($Suite -in @("Baseline", "Green", "W2")) {
    # [G19 2026-11-11] $core 原为 <repo>\src\core，[140 B3] 把测试宿主迁到
    # tests/core 后 test_cache/test_bigread/test_sparse_miss 的 .c 一个都不在
    # src/core → build-* 全部 exit=2、run-* 全部 exit=2（实测 unexpected=6）。
    # 现在从 manifest 的 targets.core.workDir 派生，与构建系统同一事实源。
    . (Join-Path $PSScriptRoot 'qa_paths.ps1')
    $coreTarget = $Script:QaManifest.targets.core
    if (-not $coreTarget.workDir) {
        Write-Output "MISSING manifest targets.core.workDir - cannot locate core sources"
        exit 1
    }
    $core = Join-Path $repoRoot ($coreTarget.workDir -replace '/', '\')
    if (-not (Test-Path -LiteralPath $core)) {
        Write-Output "MISSING core source dir: $core (manifest targets.core.workDir=$($coreTarget.workDir))"
        exit 1
    }
    # 编译输入仍在 $core，但链接产物统一落到 manifest 声明的 outDir，
    # 避免再出现"宿住在 A、产物在 B"的第二套布局。
    $coreBuild = Join-Path $repoRoot ($coreTarget.outDir -replace '/', '\')
    Write-Output "core sources: $core"
    Write-Output "core build  : $coreBuild"

    $vc = "C:\Program1\VC\Tools\MSVC\14.44.35207"
    $sdk = "C:\Program Files (x86)\Windows Kits\10"
    $sdkVersion = "10.0.26100.0"
    $env:INCLUDE = "$vc\include;$sdk\Include\$sdkVersion\ucrt;$sdk\Include\$sdkVersion\um;$sdk\Include\$sdkVersion\shared"
    $env:LIB = "$vc\lib\x64;$sdk\Lib\$sdkVersion\ucrt\x64;$sdk\Lib\$sdkVersion\um\x64"
    $env:PATH = "$vc\bin\Hostx64\x64;$env:SystemRoot\System32;$env:SystemRoot;$env:PATH"
    New-Item -ItemType Directory -Force $coreBuild | Out-Null
    $cl = Join-Path $vc "bin\Hostx64\x64\cl.exe"
    # 引擎 TU 在 src/core（不是 $core）。
    $engDir = Join-Path $srcRoot "core"
    foreach ($test in @("test_cache", "test_bigread", "test_sparse_miss")) {
        # [142] engine split into six TUs (cache.c + five domain files).
        $sources = @("$core\$test.c", "$engDir\cache.c", "$engDir\cache_stats.c",
                     "$engDir\cache_read.c", "$engDir\cache_write.c",
                     "$engDir\cache_flush.c", "$engDir\cache_evict.c")
        if ($test -eq "test_sparse_miss") { $sources += "$engDir\read_plan.c" }
        $clArgs = @("/nologo", "/O2", "/W4", "/utf-8", "/I$engDir", "/I$core") + $sources +
                  @("/Fe:$coreBuild\$test.exe", "/Fo:$coreBuild\")
        Invoke-Native "build-$test" $cl $clArgs
    }
    Invoke-Native "run-test_cache" (Join-Path $coreBuild "test_cache.exe") @()
    Invoke-Native "run-test_bigread" (Join-Path $coreBuild "test_bigread.exe") @()
    Copy-Item (Join-Path $OutputRoot "run-test_cache.stdout.txt") (Join-Path $coreBuild "rel_test_cache.out") -Force
    Copy-Item (Join-Path $OutputRoot "run-test_cache.stderr.txt") (Join-Path $coreBuild "rel_test_cache.err") -Force
    Copy-Item (Join-Path $OutputRoot "run-test_bigread.stdout.txt") (Join-Path $coreBuild "rel_test_bigread.out") -Force
    Copy-Item (Join-Path $OutputRoot "run-test_bigread.stderr.txt") (Join-Path $coreBuild "rel_test_bigread.err") -Force
    # W2: the shared read-plan harness is Green while preserving a legacy Red control.
    Invoke-Native "run-test_sparse_miss-w2" (Join-Path $coreBuild "test_sparse_miss.exe") @()

    $testProject = Join-Path $srcRoot "ui\controller\RAMBlockCache.Controller.Tests\RAMBlockCache.Controller.Tests.csproj"
    $expectedController = if ($Suite -eq "Baseline") { 1 } else { 0 }
    Invoke-Native "build-controller-tests-$($Suite.ToLowerInvariant())" $DotNet @("build", $testProject, "-c", "Release", "--no-incremental")
    Invoke-Native "controller-tests-$($Suite.ToLowerInvariant())" $DotNet @("run", "--project", $testProject, "-c", "Release", "--no-build") $expectedController

    if ($Suite -in @("Green", "W2")) {
        $ctl = Join-Path $srcRoot "ctl"
        Invoke-Native "build-bench-policy" $cl @("/nologo", "/utf-8", "/O2", "/W4", "/I$ctl", "$ctl\test_bench_policy.c", "$ctl\bench_policy.c", "/Fe:$ctl\test_bench_policy.exe", "/Fo:$ctl\")
        Invoke-Native "run-bench-policy" (Join-Path $ctl "test_bench_policy.exe") @()
        Invoke-Native "build-cli" $cl @("/nologo", "/utf-8", "/O2", "/W3", "$ctl\ramboostctl.c", "$ctl\bench_policy.c", "/Fe:$ctl\ramboostctl_w0w1.exe", "/Fo:$ctl\", "/link", "shell32.lib")
        $wpf = Join-Path $srcRoot "ui\controller\RAMBlockCache.Controller\RAMBlockCache.Controller.csproj"
        Invoke-Native "build-wpf" $DotNet @("build", $wpf, "-c", "Release", "--no-incremental")
    }
}

if ($Suite -in @("Baseline", "Gates", "W2")) {
    $python = "C:\Python315\python.exe"
    $scripts = Join-Path $srcRoot "scripts"
    Invoke-Native "gate-check_repo" $python @((Join-Path $scripts "check_repo.py"))
    Invoke-Native "gate-check_proto_sync" $python @((Join-Path $scripts "check_proto_sync.py"), "--strict")
    Invoke-Native "gate-refcount_model" $python @((Join-Path $repoRoot "deliverables\gstack\model\refcount_model.py"))
    Invoke-Native "gate-check_xaml_resources" $python @((Join-Path $scripts "check_xaml_resources.py"))
    Invoke-Native "gate-check_ps_syntax" "powershell.exe" @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $scripts "check_ps_syntax.ps1"))
    Invoke-Native "gate-check_signature" "powershell.exe" @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $scripts "check_signature.ps1"))
}

Write-Output "QA_SUMMARY suite=$Suite unexpected_failures=$script:UnexpectedFailures output=$OutputRoot"
exit $script:UnexpectedFailures
