# qa_build_core_20261020.ps1 — QA independent build of RAMBlockCache core tests.
# Pinned toolchain (same as engineer manual build):
#   MSVC 14.44.35207 @ C:\Program1 + WinSDK 10.0.26100.0
# Output: <repo>\src\core\build\qa-20261020 (QA-owned; engineer artifacts untouched)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$msvc = "C:\Program1\VC\Tools\MSVC\14.44.35207"
$kit  = "C:\Program Files (x86)\Windows Kits\10"
$out  = Join-Path $root "src\core\build\qa-20261020"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$cl = Join-Path $msvc "bin\Hostx64\x64\cl.exe"
if (-not (Test-Path -LiteralPath $cl)) { throw "cl.exe not found: $cl" }

$inc = @(
    "/I$msvc\include",
    "/I$kit\Include\10.0.26100.0\ucrt",
    "/I$kit\Include\10.0.26100.0\um",
    "/I$kit\Include\10.0.26100.0\shared",
    "/I$root\src\core",
    "/I$root\src\common",
    "/I$root\src\core\usertest"
)
$libp = @(
    "/LIBPATH:$msvc\lib\x64",
    "/LIBPATH:$kit\Lib\10.0.26100.0\ucrt\x64",
    "/LIBPATH:$kit\Lib\10.0.26100.0\um\x64"
)
$cflags = @("/nologo","/O2","/W4","/utf-8","/MT")

function Invoke-Cl([string[]]$ClArgs) {
    $of = "$env:TEMP\qa_cl_out.txt"
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $cl $ClArgs 2>&1 | Out-File -FilePath $of -Encoding utf8
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ rc = $rc; out = @(Get-Content $of -ErrorAction SilentlyContinue); err = @() }
}

Write-Output "=== compiling cache.c (plain) ==="
$r = Invoke-Cl (@($cflags) + $inc + @("/c","$root\src\core\cache.c","/Fo:$out\cache.obj"))
$r.out | Select-Object -First 10
Write-Output "cache_plain_rc=$($r.rc)"
if ($r.rc -ne 0) { throw "cache.c plain compile failed" }

Write-Output "=== compiling cache.c (/DRB_EVICT_HOOK_EXTERNAL) ==="
$r = Invoke-Cl (@($cflags) + $inc + @("/DRB_EVICT_HOOK_EXTERNAL","/c","$root\src\core\cache.c","/Fo:$out\cache_ew.obj"))
$r.out | Select-Object -First 10
Write-Output "cache_ew_rc=$($r.rc)"
if ($r.rc -ne 0) { throw "cache.c evict-wire compile failed" }

$tests = @("test_auditfix","test_bigread","test_cache","test_counter","test_counter_seed",
           "test_evict_wire","test_flush_many","test_geom","test_mixed","test_parflush",
           "test_regress","test_release","test_v5","test_v6",
           "edge_v3","edge_v4","stress_v3","stress_v4","test_write_ex","test_wsconc","offchk")
$ok = 0
foreach ($t in $tests) {
    if ($t -eq "test_evict_wire") { $co = "$out\cache_ew.obj" } else { $co = "$out\cache.obj" }
    $log = "$out\$t.cl.log"
    $r = Invoke-Cl (@($cflags) + $inc + @("/c","$root\src\core\$t.c","/Fo:$out\$t.obj"))
    if ($r.rc -ne 0) {
        Write-Output "$t`: COMPILE FAIL ($($r.rc))"
        ($r.out) | Select-Object -Last 8
        continue
    }
    $r2 = Invoke-Cl (@($cflags) + $inc + @("$out\$t.obj",$co,"/Fe:$out\$t.exe","/Fm:$out\$t.map") + @("/link") + $libp)
    if ($r2.rc -ne 0) {
        Write-Output "$t`: LINK FAIL ($($r2.rc))"
        ($r2.out) | Select-Object -Last 8
        continue
    }
    $warn = @($r2.out | Where-Object { $_ -match "warning" }).Count
    Write-Output "$t`: BUILD OK (warnings=$warn)"
    $ok++
}
Write-Output "=== binaries built: $ok / $($tests.Count) ==="
