# update-deploy-pin.ps1
# Re-register the -ExpectSha pin inside RB-sign-deploy-150.ps1 with the hash of the
# CURRENT unsigned build output.
#
# Why this exists: before 154 the driver build was NOT bit-reproducible (PE
# TimeDateStamp + CodeView RSDS Age moved on every link), so every rebuild
# invalidated the pin and RB-sign-deploy-150.cmd refused with "artifact drifted
# from the reviewed build". That refusal is correct behaviour (R16: never sign an
# artifact whose hash was not reviewed) - it was just noisy.
#
# As of 2026-11-12 the build IS deterministic: sources.manifest.json carries
# /Brepro on both cl and link, and build.ps1 deletes the stale .pdb before
# linking (otherwise link.exe reuses it and bumps RSDS Age). Two consecutive
# rebuilds now produce the same sha256 - verified. So a pin mismatch is once
# again a REAL signal: sources changed, toolchain changed, or the artifact was
# swapped. Do not "fix" a mismatch by widening the pin.
#
# This script is therefore the single documented way to move the pin, and it
# keeps a side-car state file (deliverables/.deploy-pin-state.json) holding a
# per-file sha256 of everything that decides the bytes: the .c/.h sources plus
# the build inputs (sources.manifest.json, build.ps1, paths.ps1). Behaviour:
#   * sources identical, artifact hash moved -> something outside the
#     fingerprint moved (toolchain/env). Pin is updated, but this is a warning
#     sign worth reading, not a routine event.
#   * sources changed -> REFUSE unless -AcceptSourceChange is given, and print
#     exactly which files moved so the caller can see what they are blessing.
#   * first run (no state file) -> registers a baseline.
#
# Rule of thumb: run this ONCE, as the last step before deploying. If you rebuild
# afterwards, run it again.
#
# ASCII-only + BOM by design (Gate18 check_ps_syntax).

param(
    [string]$Script = 'RB-sign-deploy-150.ps1',
    [switch]$AcceptSourceChange
)

$ErrorActionPreference = 'Stop'

$Deliverables = $PSScriptRoot
$Root         = Split-Path -Parent $Deliverables
$Built        = Join-Path $Root 'src\driver\x64\Release\RAMBlockCache.sys'
$Target       = Join-Path $Deliverables $Script
$StatePath    = Join-Path $Deliverables '.deploy-pin-state.json'

if (-not (Test-Path -LiteralPath $Built)) {
    Write-Host "[X] built artifact missing: $Built"
    Write-Host "    run: build\build.ps1 -Target driver"
    exit 2
}
if (-not (Test-Path -LiteralPath $Target)) {
    Write-Host "[X] deploy script missing: $Target"
    exit 2
}

# Same source set the deploy script uses for its staleness check (keep in sync
# with RB-sign-deploy-150.ps1 preflight, around line 82).
$excludeRe = '\\build\\|\\obj\\|\\bin\\|\\Release\\'
$srcFiles = @(Get-ChildItem -Path (Join-Path $Root 'src\driver'), (Join-Path $Root 'src\core') `
        -Recurse -Include *.c, *.h -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch $excludeRe } |
        Sort-Object FullName)
if ($srcFiles.Count -eq 0) {
    Write-Host "[X] could not enumerate driver/core sources"
    exit 2
}

# [154] The .c/.h files are not the whole story: the manifest carries every
# compile/link flag, build.ps1 drives the invocation and paths.ps1 locates the
# toolchain. Flip a flag (e.g. add /Brepro) and the artifact changes while the
# source fingerprint stays put - which used to let the pin be re-registered
# silently. Fold them into the same fingerprint.
$buildInputs = @('build\sources.manifest.json', 'build\build.ps1', 'build\paths.ps1')
foreach ($bi in $buildInputs) {
    $full = Join-Path $Root $bi
    if (Test-Path -LiteralPath $full) { $srcFiles += Get-Item -LiteralPath $full }
    else { Write-Host "[!] build input missing, not fingerprinted: $bi" }
}
$srcFiles = @($srcFiles | Sort-Object FullName)

$srcMap = [ordered]@{}
foreach ($f in $srcFiles) {
    $rel = $f.FullName.Substring($Root.Length).TrimStart('\', '/') -replace '\\', '/'
    $srcMap[$rel] = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$sb = New-Object System.Text.StringBuilder
foreach ($k in $srcMap.Keys) { [void]$sb.Append($k).Append(' ').Append($srcMap[$k]).Append("`n") }
$sha256 = [Security.Cryptography.SHA256]::Create()
try {
    $srcBytes = [Text.Encoding]::UTF8.GetBytes($sb.ToString())
    $srcFp = -join ($sha256.ComputeHash($srcBytes) | ForEach-Object { $_.ToString('x2') })
} finally { $sha256.Dispose() }

$artSha = (Get-FileHash -LiteralPath $Built -Algorithm SHA256 | Select-Object -ExpandProperty Hash).ToLowerInvariant()
$artLen = (Get-Item -LiteralPath $Built).Length

Write-Host ("artifact : {0}" -f $Built)
Write-Host ("size     : {0}" -f $artLen)
Write-Host ("sha256   : {0}" -f $artSha)
Write-Host ("sources  : {0} files, fp {1}" -f $srcMap.Count, $srcFp)

# ---------------------------------------------------------------- state compare
$prevSrc = $null
$prevMap = $null
$firstRun = $true
if (Test-Path -LiteralPath $StatePath) {
    try {
        $state = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json
        $prevSrc = [string]$state.srcFingerprint
        $prevMap = $state.files
        $firstRun = $false
    } catch {
        Write-Host "[!] state file unreadable, treating as first run: $($_.Exception.Message)"
    }
}

if ($firstRun) {
    Write-Host "[i] first run - registering baseline"
} elseif ($prevSrc -ne $srcFp) {
    Write-Host ""
    Write-Host "[!] SOURCES CHANGED since the last pin registration" -ForegroundColor Yellow
    $shown = 0
    if ($prevMap) {
        foreach ($k in $srcMap.Keys) {
            $old = $null
            if ($prevMap.PSObject.Properties[$k]) { $old = [string]$prevMap.PSObject.Properties[$k].Value }
            if ($old -ne $srcMap[$k]) {
                if ($old) { Write-Host ("    M  {0}" -f $k) } else { Write-Host ("    A  {0}" -f $k) }
                $shown++
            }
        }
        foreach ($p in $prevMap.PSObject.Properties) {
            if (-not $srcMap.Contains($p.Name)) { Write-Host ("    D  {0}" -f $p.Name); $shown++ }
        }
    }
    if ($shown -eq 0) { Write-Host "    (no per-file diff available - fingerprint only)" }
    if (-not $AcceptSourceChange) {
        Write-Host ""
        Write-Host "[X] refusing to move the pin across a source change." -ForegroundColor Red
        Write-Host "    Review the files above, then re-run with -AcceptSourceChange."
        exit 4
    }
    Write-Host "[!] -AcceptSourceChange given - blessing the new source set"
}

# ---------------------------------------------------------------- update ps1 pin
$text = [IO.File]::ReadAllText($Target)
$rx   = [regex]"\(\s*\[string\]\s*\`$ExpectSha\s*=\s*'([0-9a-fA-F]{64})'\s*\)"
$m    = $rx.Match($text)
if (-not $m.Success) {
    Write-Host "[X] could not find the -ExpectSha parameter block in $Target"
    Write-Host "    expected shape: param([string]`$ExpectSha = '<64 hex>')"
    exit 3
}

$oldPin = $m.Groups[1].Value
if ($oldPin -ne $artSha) {
    $new = $text.Replace($m.Value, "([string]`$ExpectSha = '$artSha')")
    if ($new -eq $text) {
        Write-Host "[X] replacement produced no change (unexpected)"
        exit 3
    }
    [IO.File]::WriteAllText($Target, $new, (New-Object Text.UTF8Encoding($true)))
    Write-Host ("[ok] pin updated in {0}" -f $Script)
    Write-Host ("     old: {0}" -f $oldPin)
    Write-Host ("     new: {0}" -f $artSha)
} else {
    Write-Host "[ok] pin already up to date - nothing to do"
}

# ---------------------------------------------------------------- persist state
$stateObj = [ordered]@{
    srcFingerprint = $srcFp
    artifactSha256 = $artSha
    artifactSize   = $artLen
    fileCount      = $srcMap.Count
    updatedUtc     = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    files          = $srcMap
}
$json = ($stateObj | ConvertTo-Json -Depth 4)
[IO.File]::WriteAllText($StatePath, $json, (New-Object Text.UTF8Encoding($false)))
Write-Host ("[ok] state written: {0}" -f $StatePath)

Write-Host ""
Write-Host "Next: run RB-sign-deploy-150.cmd as administrator (one UAC), then reboot once."
exit 0
