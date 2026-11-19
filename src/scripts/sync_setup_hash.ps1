# sync_setup_hash.ps1 -- post-UAC: sync the NEW Setup AND sys SHA256/size into docs.
# 2026-11-06, see deliverables/71-harvest-closeout-2026-11-06.md section 6.
# Updated 2026-11-07 (qa-s2-preflight BLOCKER-2/3):
#   * Added -RequireSetupChanged for the post-UAC call. Without it, running the
#     tool BEFORE the Setup is re-signed takes the idempotent "ALREADY IN SYNC"
#     branch, prints success, writes nothing, and section 7 of
#     verify_final_reboot.ps1 then fails on a stale README hash.
#   * Added the two SYS baseline rows (README.md / 67). They were still pinned
#     to ed1bd99c... -- one generation behind even the current 63ad0fa8... --
#     and 67 #9 said "sys 未变", which becomes false once S2 rebuilds + re-signs
#     the driver. sync_anchors.ps1 only touches the three DEPLOY anchors and
#     never fixes these documentation rows.
#
# WHY: Authenticode signing changes both the file hash and its size. Gate5 only
# verifies the SYS hash and its three anchors; it does NOT check the Setup hash.
# So after the UAC signs the rebuilt Setup, the "current baseline" rows in
# README.md and 67-freeze-closure would otherwise stay stale and mislead users.
#
# SAFETY:
#   * Reads hashes/sizes from the actual dist artifacts (single source of truth)
#     and cross-checks the FINAL_SETUP_SHA256 recorded by the UAC chain.
#   * Refuses to write unless BOTH artifacts have a Valid Authenticode signature.
#   * NEVER touches README.txt (one of the three SYS hash anchors; editing it
#     breaks gate5). It only READS it for the read-back assertion.
#   * Only rewrites the 5 "current baseline" rows. Historical ledger rows are
#     deliberately left alone.
#   * Preserves each file's original encoding: README.md is UTF-8 no BOM + CRLF,
#     67-freeze-closure is UTF-8 no BOM + LF.
#   * Idempotent, with three-state output and read-back assertions.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\sync_setup_hash.ps1 [-RequireSetupChanged]
param(
    [switch]$RequireSetupChanged
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$setup = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
$sys   = Join-Path $root 'dist\RAMBlockCache.sys'
# Dynamic current-baseline resolution: parse the CURRENT rows from the two
# baseline docs instead of hardcoding any generation's SHA/size. Both docs must
# agree (fail-closed) before a single byte is edited.
$hashLog = 'C:\ProgramData\rb-deploy\batch-hashes.txt'
$readme  = Join-Path $root 'README.md'
$d67     = Join-Path $root 'deliverables\67-freeze-closure-2026-11-06.md'
foreach ($p in @($readme, $d67, $setup, $sys)) {
    if (-not (Test-Path -LiteralPath $p)) { Write-Host ('[FAIL] missing: ' + $p); exit 1 }
}
$enc = New-Object Text.UTF8Encoding($false)
$readmeText = [IO.File]::ReadAllText($readme, $enc)
$d67Text    = [IO.File]::ReadAllText($d67, $enc)

# --- Setup baseline -------------------------------------------------------
$rxReadme = [regex]'\|\s*`dist/RAMBlockCache-Setup\.exe`\s*\|\s*`([0-9a-fA-F]{64})`\s*·\s*\*\*签名 Valid\*\*\s*\|'
$rx67     = [regex]'\|\s*Setup\s*\|\s*`([0-9a-fA-F]{64})`\s*·\s*([0-9,]+)\s*B\s*·\s*\*\*签名 Valid\*\*\s*\|'
$rm = $rxReadme.Match($readmeText)
$m67 = $rx67.Match($d67Text)
if (-not $rm.Success -or -not $m67.Success) { Write-Host '[FAIL] cannot parse current Setup baseline rows'; exit 1 }
$oldSha = $rm.Groups[1].Value.ToLower()
$old67Sha = $m67.Groups[1].Value.ToLower()
$oldSize = $m67.Groups[2].Value
if ($oldSha -ne $old67Sha) { Write-Host ('[FAIL] baseline sha disagrees: README=' + $oldSha + ' 67=' + $old67Sha); exit 1 }
Write-Host ('[info] current Setup baseline: sha=' + $oldSha + ' size=' + $oldSize)

# --- Sys baseline (BLOCKER-3) ---------------------------------------------
$rxReadmeSys = [regex]'\| `dist/RAMBlockCache\.sys` \| `[0-9a-fA-F]{64}` · \*\*签名 Valid\*\*[^|]*\|'
$rx67Sys     = [regex]'\| sys \| `[0-9a-fA-F]{64}` · [0-9,]+ B · \*\*签名 Valid\*\*[^|]*\|'
$rmSys = $rxReadmeSys.Match($readmeText)
$m67Sys = $rx67Sys.Match($d67Text)
if (-not $rmSys.Success -or -not $m67Sys.Success) { Write-Host '[FAIL] cannot parse current sys baseline rows'; exit 1 }
$readmeSysLine = $rmSys.Value
$d67SysLine    = $m67Sys.Value
$rxSha = [regex]'[0-9a-fA-F]{64}'
$oldSysSha = $rxSha.Match($readmeSysLine).Value.ToLower()
$old67SysSha = $rxSha.Match($d67SysLine).Value.ToLower()
if ($oldSysSha -ne $old67SysSha) { Write-Host ('[FAIL] sys baseline disagrees: README=' + $oldSysSha + ' 67=' + $old67SysSha); exit 1 }
Write-Host ('[info] current sys baseline  : sha=' + $oldSysSha + ' (dist may already be newer)')

# --- Actual signed artifacts ----------------------------------------------
$sigSetup = Get-AuthenticodeSignature -LiteralPath $setup
if ($sigSetup.Status -ne 'Valid') { Write-Host ("[FAIL] Setup signature is " + $sigSetup.Status + ", refuse to sync an unsigned artifact"); exit 1 }
$sigSys = Get-AuthenticodeSignature -LiteralPath $sys
if ($sigSys.Status -ne 'Valid') { Write-Host ("[FAIL] sys signature is " + $sigSys.Status + ", refuse to sync an unsigned artifact"); exit 1 }
$newSha = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLower()
$newSize = (Get-Item -LiteralPath $setup).Length.ToString('N0', [Globalization.CultureInfo]::InvariantCulture)
$sysShaNow = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash.ToLower()
$sysSizeNow = (Get-Item -LiteralPath $sys).Length.ToString('N0', [Globalization.CultureInfo]::InvariantCulture)
Write-Host ("[info] setup sha256=" + $newSha + " size=" + $newSize)
Write-Host ("[info] sys   sha256=" + $sysShaNow + " size=" + $sysSizeNow)

# Post-UAC guard: the Setup MUST have changed. Otherwise the UAC chain did not
# complete and an "ALREADY IN SYNC" would be a silent false green.
if ($RequireSetupChanged -and $oldSha -eq $newSha) {
    Write-Host '[FAIL] -RequireSetupChanged: dist Setup hash is unchanged from the documented baseline -- the UAC replay may not have run'
    exit 1
}

# Cross-check the value recorded during the UAC run, if present.
# finalize_geom_rebuild.ps1 now treats the ledger append as FATAL and verifies
# its own read-back, so a mismatch here is a real defect, not a warning.
if (Test-Path -LiteralPath $hashLog) {
    $recorded = @(Get-Content -LiteralPath $hashLog | Where-Object { $_ -match '^FINAL_SETUP_SHA256=' }) | Select-Object -Last 1
    if ($recorded) {
        $rv = ($recorded -split '=', 2)[1].Trim().ToLower()
        if ($rv -ne $newSha) {
            Write-Host ("[FAIL] batch-hashes.txt FINAL_SETUP_SHA256=" + $rv + " does not match dist=" + $newSha)
            exit 1
        }
        Write-Host '[ok] cross-check with batch-hashes.txt FINAL_SETUP_SHA256: MATCH'
    } else {
        Write-Host '[warn] no FINAL_SETUP_SHA256 in batch-hashes.txt (UAC may not have run) - dist is authoritative'
    }
}

# --- Build the exact row replacements ------------------------------------
$newReadmeSysLine = $rxSha.Replace($readmeSysLine, $sysShaNow, 1)
$new67SysLine = $rxSha.Replace($d67SysLine, $sysShaNow, 1)
$rxSize = [regex]'[0-9,]+ B'
$new67SysLine = $rxSize.Replace($new67SysLine, ($sysSizeNow + ' B'), 1)
$new67SysLine = $new67SysLine.Replace('本轮未变，三锚点不动', '本轮重编重签，三锚点已同步')

# The 67 #9 "发布物" line changes BOTH its 8-char Setup prefix and its sys
# wording. Parse the CURRENT line instead of pinning the old "sys 未变" text,
# otherwise the second (idempotent) run cannot find its own output and fails.
$rxPublish = [regex]'\*\*发布物\*\*：`dist/RAMBlockCache-Setup\.exe`（`[0-9a-fA-F]{8}…`，Valid）；`dist/RAMBlockCache\.sys`[^\r\n]*。'
$mPub = $rxPublish.Match($d67Text)
if (-not $mPub.Success) { Write-Host '[FAIL] cannot parse 67 #9 publish line'; exit 1 }
$publishLine = $mPub.Value
$newPublishLine = '**发布物**：`dist/RAMBlockCache-Setup.exe`（`' + $newSha.Substring(0,8) + '…`，Valid）；`dist/RAMBlockCache.sys` 已重编重签（`' + $sysShaNow.Substring(0,8) + '…`）。'

$targets = @(
    @{
        Path = $readme; NL = "`r`n"
        Old  = '| `dist/RAMBlockCache-Setup.exe` | `' + $oldSha + '` · **签名 Valid** |'
        New  = '| `dist/RAMBlockCache-Setup.exe` | `' + $newSha + '` · **签名 Valid** |'
    },
    @{
        Path = $d67; NL = "`n"
        Old  = '| Setup | `' + $oldSha + '` · ' + $oldSize + ' B · **签名 Valid** |'
        New  = '| Setup | `' + $newSha + '` · ' + $newSize + ' B · **签名 Valid** |'
    },
    @{
        Path = $d67; NL = "`n"
        Old  = $publishLine
        New  = $newPublishLine
    },
    @{
        Path = $readme; NL = "`r`n"
        Old  = $readmeSysLine
        New  = $newReadmeSysLine
    },
    @{
        Path = $d67; NL = "`n"
        Old  = $d67SysLine
        New  = $new67SysLine
    }
)

# Preflight: every old row must exist exactly once. Nothing is written unless
# all five are found. This is the "fail closed before any edit" guarantee.
foreach ($t in $targets) {
    $txt = [IO.File]::ReadAllText($t.Path, $enc)
    $hits = @([regex]::Matches($txt, [regex]::Escape($t.Old)))
    if ($hits.Count -ne 1) {
        Write-Host ("[FAIL] " + (Split-Path -Leaf $t.Path) + ": expected exactly 1 old row, found " + $hits.Count)
        Write-Host ("       row: " + $t.Old)
        exit 1
    }
}
Write-Host ('[ok] preflight: ' + $targets.Count + ' current-baseline rows found exactly once each')

# Apply (each path has multiple rows -> do all replacements on one in-memory text).
$byPath = @{}
foreach ($t in $targets) { if (-not $byPath.ContainsKey($t.Path)) { $byPath[$t.Path] = $t.NL } }
$changed = 0
foreach ($p in $byPath.Keys) {
    $txt = [IO.File]::ReadAllText($p, $enc)
    $orig = $txt
    foreach ($t in @($targets | Where-Object { $_.Path -eq $p })) {
        $txt = $txt.Replace($t.Old, $t.New)
    }
    $txt = $txt -replace "`r`n", "`n" -replace "`n", $byPath[$p]
    if ($txt -ne $orig) {
        [IO.File]::WriteAllText($p, $txt, $enc)
        $changed++
        Write-Host ("[ok] updated " + (Split-Path -Leaf $p))
    } else {
        Write-Host ("[ok] already in sync " + (Split-Path -Leaf $p))
    }
}

# Read-back: new hashes must appear; old hashes must be gone when a real change
# was requested; README.txt SYS anchor must still match the CURRENT dist sys.
$fail = 0
foreach ($p in @($readme, $d67)) {
    $txt = [IO.File]::ReadAllText($p, $enc)
    if ($txt.IndexOf($newSha, [StringComparison]::Ordinal) -lt 0) { Write-Host ("[FAIL] read-back missing new Setup sha in " + $p); $fail++ }
    if ($txt.IndexOf($sysShaNow, [StringComparison]::Ordinal) -lt 0) { Write-Host ("[FAIL] read-back missing current sys sha in " + $p); $fail++ }
    if ($oldSha -ne $newSha -and $txt.IndexOf($oldSha, [StringComparison]::Ordinal) -ge 0) {
        Write-Host ("[FAIL] read-back still has old Setup sha in " + $p); $fail++
    }
    if ($oldSysSha -ne $sysShaNow -and $txt.IndexOf($oldSysSha, [StringComparison]::Ordinal) -ge 0) {
        Write-Host ("[FAIL] read-back still has old sys sha in " + $p); $fail++
    }
}
$rt = Join-Path $root 'README.txt'
$anchorLine = Select-String -LiteralPath $rt -Pattern $sysShaNow -SimpleMatch -Quiet
if (-not $anchorLine) { Write-Host ('[FAIL] README.txt sys anchor does not match current dist sys ' + $sysShaNow); $fail++ }
else { Write-Host ('[ok] README.txt sys anchor intact and matches current sys ' + $sysShaNow) }
if ($fail -gt 0) { Write-Host ("[FAIL] read-back failed (" + $fail + ")"); exit 1 }

Write-Host ("== sync_setup_hash: " + $(if ($changed -gt 0) { 'UPDATED' } else { 'ALREADY IN SYNC' }) + " ==")
Write-Host 'NOTE: historical ledger rows intentionally untouched; see 71-harvest-closeout section 6.'
exit 0
