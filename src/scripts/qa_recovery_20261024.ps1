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
    & $FilePath @Arguments > $stdout 2> $stderr
    $actual = $LASTEXITCODE
    [System.IO.File]::WriteAllText((Join-Path $OutputRoot "$Name.exit.txt"), [string]$actual, [System.Text.Encoding]::ASCII)
    Write-Output "$Name exit=$actual expected=$ExpectedExit"
    if ($actual -ne $ExpectedExit) { $script:UnexpectedFailures++ }
}

if ($Suite -in @("Baseline", "Green", "W2")) {
    $vc = "C:\Program1\VC\Tools\MSVC\14.44.35207"
    $sdk = "C:\Program Files (x86)\Windows Kits\10"
    $sdkVersion = "10.0.26100.0"
    $env:INCLUDE = "$vc\include;$sdk\Include\$sdkVersion\ucrt;$sdk\Include\$sdkVersion\um;$sdk\Include\$sdkVersion\shared"
    $env:LIB = "$vc\lib\x64;$sdk\Lib\$sdkVersion\ucrt\x64;$sdk\Lib\$sdkVersion\um\x64"
    $env:PATH = "$vc\bin\Hostx64\x64;$env:SystemRoot\System32;$env:SystemRoot;$env:PATH"
    $core = Join-Path $srcRoot "core"
    $coreBuild = Join-Path $core "build"
    New-Item -ItemType Directory -Force $coreBuild | Out-Null
    $cl = Join-Path $vc "bin\Hostx64\x64\cl.exe"
    foreach ($test in @("test_cache", "test_bigread", "test_sparse_miss")) {
        $sources = @("$core\$test.c", "$core\cache.c")
        if ($test -eq "test_sparse_miss") { $sources += "$core\read_plan.c" }
        $clArgs = @("/nologo", "/O2", "/W4", "/utf-8", "/I$core") + $sources + @("/Fe:$coreBuild\$test.exe", "/Fo:$coreBuild\")
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
