# test-counter-parse.ps1  [JUDGE SELFTEST - keep: this is the proof that the criteria have teeth]
# WHAT IT PROVES: the shipped status parser: LF/CRLF/mixed endings, trailing whitespace, reversed counters, unparseable input all resolve to a value or an explicit MISSING, never to 0
# Extracts Get-CounterSnapshot and Format-CounterDelta from RB-write-load.ps1
# and feeds them REAL `ramboostctl status` lines (copied from ramboostctl.c:90-104
# format strings) so the regexes are proven against the actual wire format rather
# than against a string I imagined.
#
# Round 2 additions: the parser now matches PER LINE (-split "\r?\n"), because
# the C side emits a bare '\n' and a console transforms it. So the same payload
# must parse identically whether it arrives as LF-only, CRLF-only, or mixed --
# and the raw matched line must be preserved as evidence.

param(
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) 'RB-write-load.ps1')
)

$ErrorActionPreference = 'Stop'
$text = [IO.File]::ReadAllText($Source)
$s = $text.IndexOf('function Get-CounterSnapshot')
$e = $text.IndexOf('function Write-RealFile')
if ($s -lt 0 -or $e -lt $s) { Write-Host 'SELFTEST FAIL: cannot locate counter functions'; exit 90 }
$block = $text.Substring($s, $e - $s)

# $name is used by Format-CounterDelta's PSObject property lookup; nothing else needed.
Invoke-Expression $block

# Verbatim shape of `ramboostctl status` per ramboostctl.c:90-104.
$linesBefore = @(
    '  缓存大小: 8192 MB (自动预算)   当前占用: 1.20 GB   脏数据: 0.00 MB'
    '  读请求: 4000   读命中(次数): 99.50%   读命中(字节): 84.45%'
    '  写请求: 1500   写字节: 12.34 GB   缓存吸收: 3.20%'
    '  回写次数: 10   回写字节: 0.50 GB   淘汰字节: 0.20 GB'
    '  RMW 补读: 0 次   余量整理: 3 次'
)
$linesAfter = @(
    '  缓存大小: 8192 MB (自动预算)   当前占用: 1.35 GB   脏数据: 0.00 MB'
    '  读请求: 4000   读命中(次数): 99.50%   读命中(字节): 84.45%'
    '  写请求: 2333   写字节: 14.10 GB   缓存吸收: 2.80%'
    '  回写次数: 10   回写字节: 0.50 GB   淘汰字节: 0.20 GB'
    '  RMW 补读: 0 次   余量整理: 3 次'
)
# Three wire shapes, all of which the tool's stdout can legitimately take.
$statusBefore = $linesBefore -join "`n"        # bare LF: what printf emits
$statusAfter  = $linesAfter  -join "`n"
$statusBeforeCRLF = ($linesBefore -join "`r`n") + "`r`n"
$statusAfterCRLF  = ($linesAfter  -join "`r`n") + "`r`n"
# Trailing spaces / tabs after the number must still parse ('\s*' before GB).
$statusAfterNoisy = ($linesAfter -join "`n").Replace('14.10 GB', "14.10   GB `t")

$before = Get-CounterSnapshot $statusBefore
$after  = Get-CounterSnapshot $statusAfter

$fail = 0
function T([string]$what, [bool]$cond, [string]$detail = '') {
    if ($cond) { Write-Output "  ok    $what" } else { Write-Output "  FAIL  $what  $detail"; $script:fail++ }
}

Write-Output "=== parse (LF wire form) ==="
Write-Output ("  WriteBytes before = {0:N0}" -f $before.WriteBytes)
Write-Output ("  WriteBytes after  = {0:N0}" -f $after.WriteBytes)
Write-Output ("  WriteReq   before = {0}" -f $before.WriteReq)
Write-Output ("  WriteReq   after  = {0}" -f $after.WriteReq)
Write-Output ("  ReadReq    before = {0}" -f $before.ReadReq)
Write-Output ("  ReadBytes         = {0}" -f $(if ($null -eq $before.ReadBytes) { 'null (as designed)' } else { $before.ReadBytes }))
Write-Output ("  raw line captured = [{0}]" -f $before.WriteBytesLine)

Write-Output "=== assertions: LF wire form ==="
$expectBefore = [decimal]12.34 * 1GB
$expectAfter  = [decimal]14.10 * 1GB
T 'WriteBytes parsed from before' ($before.WriteBytes -eq $expectBefore) "got $($before.WriteBytes) want $expectBefore"
T 'WriteBytes parsed from after'  ($after.WriteBytes  -eq $expectAfter)  "got $($after.WriteBytes) want $expectAfter"
T 'WriteReq parsed from before'   ($before.WriteReq -eq 1500) "got $($before.WriteReq)"
T 'WriteReq parsed from after'    ($after.WriteReq  -eq 2333) "got $($after.WriteReq)"
T 'ReadReq parsed from before'    ($before.ReadReq -eq 4000) "got $($before.ReadReq)"
T 'ReadBytes stays null (not printed by the tool)' ($null -eq $before.ReadBytes)
T 'WriteBytesLine captured as evidence' ($before.WriteBytesLine -match '写字节:\s*12\.34 GB') "got [$($before.WriteBytesLine)]"
T 'WriteBytesLine has no stray CR/LF' ($before.WriteBytesLine -notmatch "[`r`n]") "got [$($before.WriteBytesLine)]"

$dW = Format-CounterDelta $before $after 'WriteBytes'
$dR = Format-CounterDelta $before $after 'ReadBytes'
$dQ = if ($null -ne $before.WriteReq -and $null -ne $after.WriteReq) { $after.WriteReq - $before.WriteReq } else { 'MISSING' }
Write-Output ("  dWrite = {0:N0} B" -f $dW)
Write-Output ("  writeReq delta = {0}" -f $dQ)
Write-Output ("  dRead  = {0}" -f $dR)

T 'dWrite is numeric' ($dW -ne 'MISSING' -and $dW -gt 0) "got $dW"
T 'dWrite matches 14.10-12.34 GB' ([math]::Abs([double]$dW - ([double]($expectAfter - $expectBefore))) -lt 1) "got $dW"
T 'writeReq delta == 833' ($dQ -eq 833) "got $dQ"
T 'dRead is MISSING (field does not exist)' ($dR -eq 'MISSING') "got $dR"

# ---- the reason -split was introduced: line ending must not matter ----------
Write-Output "=== assertions: CRLF wire form (identical result required) ==="
$bC = Get-CounterSnapshot $statusBeforeCRLF
$aC = Get-CounterSnapshot $statusAfterCRLF
$dWC = Format-CounterDelta $bC $aC 'WriteBytes'
Write-Output ("  dWrite(CRLF) = {0:N0} B" -f $dWC)
T 'CRLF: WriteBytes before identical to LF' ($bC.WriteBytes -eq $before.WriteBytes) "got $($bC.WriteBytes) want $($before.WriteBytes)"
T 'CRLF: WriteBytes after identical to LF'  ($aC.WriteBytes -eq $after.WriteBytes)  "got $($aC.WriteBytes) want $($after.WriteBytes)"
T 'CRLF: dWrite identical to LF'            ($dWC -eq $dW) "got $dWC want $dW"
T 'CRLF: WriteBytesLine still has no CR'    ($bC.WriteBytesLine -notmatch "[`r`n]") "got [$($bC.WriteBytesLine)]"

Write-Output "=== assertions: mixed endings + trailing whitespace ==="
$mixed = ($linesBefore -join "`n") + "`r`n" + ($linesAfter -join "`n")
$bM = Get-CounterSnapshot $mixed
T 'mixed LF/CRLF: first 写字节 wins (12.34)' ($bM.WriteBytes -eq $expectBefore) "got $($bM.WriteBytes)"
$noisy = Get-CounterSnapshot $statusAfterNoisy
Write-Output ("  写字节 line = [{0}]" -f ($statusAfterNoisy -split "\r?\n")[2])
T 'trailing spaces/tab before GB still parse' ($noisy.WriteBytes -eq $expectAfter) "got $($noisy.WriteBytes)"

# The failure mode this guards: a backwards counter must not become a negative delta.
$backwards = Format-CounterDelta $after $before 'WriteBytes'
T 'backwards counter -> MISSING, not negative' ($backwards -eq 'MISSING') "got $backwards"

# Null text must not throw, and must not be read as zeros.
$empty = Get-CounterSnapshot ''
T 'empty input -> all null, no throw' ($null -eq $empty.WriteBytes -and $null -eq $empty.WriteReq)

# A blob with the label but no number must not be coerced to 0.
$junk = Get-CounterSnapshot "  写请求: 12   写字节: n/a GB"
T 'unparseable 写字节 -> null, not 0' ($null -eq $junk.WriteBytes) "got $($junk.WriteBytes)"
T 'unparseable 写字节 -> dWrite MISSING' ((Format-CounterDelta $junk $junk 'WriteBytes') -eq 'MISSING')

Write-Output "=== result ==="
if ($fail -eq 0) { Write-Output 'ALL COUNTER PARSE TESTS OK' } else { Write-Output "$fail FAILED" }
exit $(if ($fail -eq 0) { 0 } else { 1 })
