# sync_setup_hash.ps1 -- post-UAC: sync the NEW Setup SHA256/size into docs.
# 2026-11-06, see deliverables/71-harvest-closeout-2026-11-06.md section 6.
#
# WHY: Authenticode signing changes both the file hash and its size. Gate5 only
# verifies the SYS hash and its three anchors; it does NOT check the Setup hash.
# So after RB-finalize-final.cmd signs the rebuilt Setup, the "current baseline"
# rows in README.md and 67-freeze-closure would otherwise stay stale at
# 7912d276... / 276,153,688 B and mislead users into distributing the wrong file.
#
# SAFETY:
#   * Reads the hash/size from the actual dist Setup (single source of truth)
#     and cross-checks the FINAL_SETUP_SHA256 recorded by finalize_final.ps1.
#   * Refuses to write unless the Setup Authenticode signature is Valid.
#   * NEVER touches README.txt (that is one of the three SYS hash anchors --
#     editing it breaks gate5 immediately).
#   * Only rewrites the 3 "current baseline" rows. Historical ledger rows
#     (KNOWN_ISSUES E8, 65 #update2, 66 #update, 64 #correction, 67 #14/#124)
#     are deliberately left alone; 71-harvest-closeout lists them for notes.
#   * Preserves each file's original encoding: README.md is UTF-8 no BOM + CRLF,
#     67-freeze-closure is UTF-8 no BOM + LF. Verified 2026-11-06 18:4x.
#   * Idempotent, with explicit three-state output and read-back assertions.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\sync_setup_hash.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$setup = Join-Path $root 'dist\RAMBlockCache-Setup.exe'
# [2026-11-06 22:5x, lead] Dynamic current-baseline resolution.
# The old script hardcoded $oldSha='7912d276...' / $oldSize='276,153,688'.
# After the 19:05 UAC those rows were already rewritten to 6acb825f..., so a
# SECOND release replay would fail preflight with "expected exactly 1 old row,
# found 0". Parse the CURRENT rows from the two baseline docs instead, and
# cross-check that both docs agree (fail-closed) before editing anything.
# This makes the tool reusable for every future release without a constant edit.
$hashLog = 'C:\ProgramData\rb-deploy\batch-hashes.txt'
$readme  = Join-Path $root 'README.md'
$d67     = Join-Path $root 'deliverables\67-freeze-closure-2026-11-06.md'
foreach ($p in @($readme, $d67)) {
    if (-not (Test-Path -LiteralPath $p)) { Write-Host ('[FAIL] baseline doc missing: ' + $p); exit 1 }
}
$rxReadme = [regex]'\|\s*`dist/RAMBlockCache-Setup\.exe`\s*\|\s*`([0-9a-fA-F]{64})`\s*·\s*\*\*签名 Valid\*\*\s*\|'
$rx67     = [regex]'\|\s*Setup\s*\|\s*`([0-9a-fA-F]{64})`\s*·\s*([0-9,]+)\s*B\s*·\s*\*\*签名 Valid\*\*\s*\|'
$rm = $rxReadme.Match([IO.File]::ReadAllText($readme, (New-Object Text.UTF8Encoding($false))))
$m67 = $rx67.Match([IO.File]::ReadAllText($d67, (New-Object Text.UTF8Encoding($false))))
if (-not $rm.Success -or -not $m67.Success) { Write-Host '[FAIL] cannot parse current Setup baseline rows'; exit 1 }
$oldSha = $rm.Groups[1].Value.ToLower()
$old67Sha = $m67.Groups[1].Value.ToLower()
$oldSize = $m67.Groups[2].Value
if ($oldSha -ne $old67Sha) { Write-Host ('[FAIL] baseline sha disagrees: README=' + $oldSha + ' 67=' + $old67Sha); exit 1 }
Write-Host ('[info] current baseline: sha=' + $oldSha + ' size=' + $oldSize)

if (-not (Test-Path -LiteralPath $setup)) { Write-Host '[FAIL] dist Setup missing'; exit 1 }
$sig = Get-AuthenticodeSignature -LiteralPath $setup
if ($sig.Status -ne 'Valid') { Write-Host ("[FAIL] Setup signature is " + $sig.Status + ", refuse to sync an unsigned artifact"); exit 1 }
$newSha = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLower()
$newSize = (Get-Item -LiteralPath $setup).Length.ToString('N0', [Globalization.CultureInfo]::InvariantCulture)
Write-Host ("[info] setup sha256=" + $newSha)
Write-Host ("[info] setup size=" + $newSize)

# Cross-check the value recorded during the UAC run, if present.
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

# Each entry: path, newline style, exact old row -> exact new row.
$readme = Join-Path $root 'README.md'
$d67 = Join-Path $root 'deliverables\67-freeze-closure-2026-11-06.md'
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
        Old  = '**发布物**：`dist/RAMBlockCache-Setup.exe`（`' + $oldSha.Substring(0,8) + '…`，Valid）；`dist/RAMBlockCache.sys` 未变。'
        New  = '**发布物**：`dist/RAMBlockCache-Setup.exe`（`' + $newSha.Substring(0,8) + '…`，Valid）；`dist/RAMBlockCache.sys` 未变。'
    }
)

# Preflight: every old row must exist exactly once. Nothing is written unless
# all three are found. This is the "fail closed before any edit" guarantee.
$enc = New-Object Text.UTF8Encoding($false)
foreach ($t in $targets) {
    if (-not (Test-Path -LiteralPath $t.Path)) { Write-Host ("[FAIL] missing " + $t.Path); exit 1 }
    $txt = [IO.File]::ReadAllText($t.Path, $enc)
    $hits = @([regex]::Matches($txt, [regex]::Escape($t.Old)))
    if ($hits.Count -ne 1) {
        Write-Host ("[FAIL] " + (Split-Path -Leaf $t.Path) + ": expected exactly 1 old row, found " + $hits.Count)
        Write-Host ("       row: " + $t.Old)
        exit 1
    }
}
Write-Host '[ok] preflight: 3 current-baseline rows found exactly once each'

# Apply (67 has two rows -> do both replacements on the same in-memory text).
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

# Read-back: new hash must appear, old hash must be gone from the two baseline
# docs. README.txt is one of the three SYS anchors: this tool must NEVER write
# it, but after a driver rebuild sync_anchors.ps1 legitimately updates its
# DefaultExpectedSha256. So verify the anchor against the CURRENT dist sys
# hash instead of a hardcoded stale value (the 19:05 build used ed1bd99c...).
$fail = 0
foreach ($p in @($readme, $d67)) {
    $txt = [IO.File]::ReadAllText($p, $enc)
    if ($txt.IndexOf($newSha, [StringComparison]::Ordinal) -lt 0) { Write-Host ("[FAIL] read-back missing new sha in " + $p); $fail++ }
    # Only assert the old hash is gone when a real change was requested.
    # Idempotent re-run (oldSha == newSha) would otherwise flag its own baseline.
    if ($oldSha -ne $newSha -and $txt.IndexOf($oldSha, [StringComparison]::Ordinal) -ge 0) {
        Write-Host ("[FAIL] read-back still has old sha in " + $p); $fail++
    }
}
$rt = Join-Path $root 'README.txt'
$sysShaNow = (Get-FileHash -LiteralPath (Join-Path $root 'dist\RAMBlockCache.sys') -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower()
$anchorLine = Select-String -LiteralPath $rt -Pattern $sysShaNow -SimpleMatch -Quiet
if (-not $anchorLine) { Write-Host ('[FAIL] README.txt sys anchor does not match current dist sys ' + $sysShaNow); $fail++ }
else { Write-Host ('[ok] README.txt sys anchor intact and matches current sys ' + $sysShaNow) }
if ($fail -gt 0) { Write-Host ("[FAIL] read-back failed (" + $fail + ")"); exit 1 }

Write-Host ("== sync_setup_hash: " + $(if ($changed -gt 0) { 'UPDATED' } else { 'ALREADY IN SYNC' }) + " ==")
Write-Host 'NOTE: historical ledger rows intentionally untouched; see 71-harvest-closeout section 6.'
exit 0
