# qa_paths.ps1 -- resolve QA binary paths from build/sources.manifest.json.
#
# [G19 2026-11-11] Why this file exists.
#
# Five QA drivers hardcoded `<repo>\src\core\build\<exe>`. That directory stopped
# being the artifact location when [140 B3] moved the test hosts from src/core to
# tests/core; the manifest has said `targets.core.outDir = "tests/core/build"`
# ever since, and 60 executables actually live there. The drivers kept pointing
# at the abandoned path, so:
#
#   qa_smoke_wsconc.ps1               exit 1  (test_wsconc.exe not found)
#   qa_sweep_wsconc.ps1               exit 1
#   qa_repeat_concurrency_20261020    exit 1  (7/7 MISSING binary)
#   qa_geom_gate.ps1                  exit 1  (3/3 prerequisites missing)
#   qa_recovery_20261024.ps1          exit 6  (6/6 build failures)
#
# They all fail closed, so nothing was silently green -- but every historical
# smoke / sweep / geometry / recovery claim in the reports names a driver that
# cannot run. That is a broken evidence chain, which is worse than a red gate:
# a red gate gets fixed, a missing reproduction path gets cited forever.
#
# Fixing it by writing `tests/core/build` into five scripts would just move the
# hardcoded path. The manifest is already the single source of truth for build
# outputs (`targets.core.outDir`), so these helpers read it instead. If the
# layout moves again, one JSON field moves with it.
#
# Usage:
#   . (Join-Path $PSScriptRoot 'qa_paths.ps1')
#   $exe = Get-QaBinary 'test_wsconc.exe'      # -> full path or throws
#   $dir = Get-QaOutDir                        # -> <repo>\tests\core\build

$ErrorActionPreference = 'Stop'

# scripts\ -> repo root is two levels up.
$Script:QaRepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Script:QaManifestPath = Join-Path $Script:QaRepoRoot 'build\sources.manifest.json'

if (-not (Test-Path -LiteralPath $Script:QaManifestPath)) {
    throw "sources.manifest.json missing: $Script:QaManifestPath (QA drivers cannot resolve artifacts)"
}

$Script:QaManifest = [IO.File]::ReadAllText($Script:QaManifestPath, [Text.Encoding]::UTF8) |
                      ConvertFrom-Json

function Get-QaCoreTarget {
    $t = $Script:QaManifest.targets.core
    if (-not $t) { throw "manifest targets.core missing -- cannot resolve QA artifacts" }
    if (-not $t.outDir) { throw "manifest targets.core.outDir missing -- cannot resolve QA artifacts" }
    return $t
}

function Get-QaOutDir {
    $t = Get-QaCoreTarget
    $dir = Join-Path $Script:QaRepoRoot ($t.outDir -replace '/', '\')
    if (-not (Test-Path -LiteralPath $dir)) {
        throw "QA output dir missing: $dir (run build/build.ps1 -Target core first)"
    }
    return $dir
}

function Get-QaBinary([string]$Name) {
    if (-not $Name) { throw "Get-QaBinary: empty name" }
    $dir = Get-QaOutDir
    $exe = Join-Path $dir $Name
    if (-not (Test-Path -LiteralPath $exe)) {
        $have = @(Get-ChildItem -LiteralPath $dir -Filter '*.exe' -ErrorAction SilentlyContinue |
                  Select-Object -ExpandProperty Name)
        throw ("QA binary missing: {0}`n  looked in: {1}`n  {2} exe(s) present: {3}" -f
               $Name, $dir, $have.Count, (($have | Sort-Object | Select-Object -First 12) -join ', '))
    }
    return $exe
}

# Same contract as Get-QaBinary but returns $null instead of throwing, for
# callers that want to report a full prerequisite table before giving up.
function Find-QaBinary([string]$Name) {
    try {
        $dir = Get-QaOutDir
        $exe = Join-Path $dir $Name
        if (Test-Path -LiteralPath $exe) { return $exe }
        return $null
    } catch {
        return $null
    }
}

function Get-QaOutDirOrNull {
    try { return Get-QaOutDir } catch { return $null }
}
