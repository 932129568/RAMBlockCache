# _build_ctl_both.ps1 -- authoritative LOCAL CLI build (S2 replay preflight).
# Pure ASCII on purpose (PS 5.1 GBK console lesson). Must stay UTF-8 BOM + CRLF.
#
# Why this exists:
#   1) _build_ctl_both.cmd omitted bench_policy.c -> LNK2019 on the three
#      RbBench* helpers used by CliFlushFileRange / cli_bench, so it produced
#      NO ramboostctl.exe at all (found 2026-11-07 during S2 preflight).
#   2) cmd<->powershell interop is blocked in this sandbox, so the release
#      chain needs a pure-PowerShell entry that can be called and verified.
#   3) vcvars64.bat is blocked (reg.exe) -> INCLUDE/LIB/PATH are set here.
#      LIB MUST contain um\x64 + ucrt\x64 + MSVC\lib\x64 (missing ucrt -> LNK1104).
#
# Fail-closed contract: after a successful link the five RB_STATS statval keys
# MUST be present in ramboostctl.exe. A stale CLI silently breaks the E1-c
# acceptance gate in deliverables/79-runtime-geometry-rebuild-2026-11-07.md
# (ramboostctl statval engine_bs), which no existing gate used to catch.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File src\ctl\_build_ctl_both.ps1
$ErrorActionPreference = 'Stop'
$ctlDir = $PSScriptRoot
$vct    = 'C:\Program1\VC\Tools\MSVC\14.44.35207'
$kit    = 'C:\Program Files (x86)\Windows Kits\10'
$cl     = Join-Path $vct 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path -LiteralPath $cl)) { throw "cl.exe not found: $cl" }

# Before touching any delivered binary, prove every source is present.
foreach ($n in @('ramboostctl.c','bench_policy.c','rbfeatctl.c')) {
    if (-not (Test-Path -LiteralPath (Join-Path $ctlDir $n))) { throw "missing source: $n" }
}

$env:INCLUDE = "$vct\include;$kit\Include\10.0.26100.0\ucrt;$kit\Include\10.0.26100.0\um;$kit\Include\10.0.26100.0\shared"
$env:LIB     = "$vct\lib\x64;$kit\Lib\10.0.26100.0\um\x64;$kit\Lib\10.0.26100.0\ucrt\x64"
$env:PATH    = "$vct\bin\Hostx64\x64;$env:PATH"

# Relative /Fe names on purpose: the repo path contains non-ASCII characters
# and cl.exe mishandles an absolute /Fe: with them. cwd is pinned below.
Push-Location -LiteralPath $ctlDir
try {
    Write-Host '== [1/2] ramboostctl.exe (+ bench_policy.c) =='
    & $cl /nologo /utf-8 /O2 /W3 ramboostctl.c bench_policy.c /Fe:ramboostctl.exe advapi32.lib bcrypt.lib
    if ($LASTEXITCODE -ne 0) { throw "ramboostctl build failed (exit=$LASTEXITCODE)" }

    Write-Host '== [2/2] rbfeatctl.exe =='
    & $cl /nologo /utf-8 /O2 /W3 rbfeatctl.c /Fe:rbfeatctl.exe advapi32.lib
    if ($LASTEXITCODE -ne 0) { throw "rbfeatctl build failed (exit=$LASTEXITCODE)" }
}
finally { Pop-Location }

# ---- fail-closed output contract -----------------------------------------
$keys = @('engine_bs','rebuild_count','rebuild_status','rebuild_in_progress','rebuild_utc')
$exeCtl = Join-Path $ctlDir 'ramboostctl.exe'
if (-not (Test-Path -LiteralPath $exeCtl)) { throw 'ramboostctl.exe missing after build' }
$txt = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exeCtl))
foreach ($k in $keys) {
    if (-not $txt.Contains($k)) { throw "ramboostctl.exe is missing statval key '$k' (stale/misbuilt binary)" }
}
Write-Host ('[ok] ramboostctl.exe carries all ' + $keys.Count + ' RB_STATS statval keys')
foreach ($n in @('ramboostctl.exe','rbfeatctl.exe')) {
    $p = Join-Path $ctlDir $n
    $i = Get-Item -LiteralPath $p
    $h = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower()
    Write-Host ("  {0} | size={1} | sha256={2}" -f $n, $i.Length, $h)
}
Write-Host '=== CTL BUILD OK ==='
