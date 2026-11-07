# qa_geom_gate.ps1 - [72/73/74 2026-11-06] >4K geometry concurrency gate (fail-closed)
# Purpose: prove the cache.c rb_write_pass2 stripe-lock fix is effective at
#   block_size 8192/16384/32768 AND that 4K stays structurally identical to
#   the noon-63 baseline. Runs test_flushpar Release + ASan on every cell.
# Output: structured JSON + explicit GATE: PASS/FAIL + process exit 0/1.
# Rules: Windows PowerShell 5.1 compatible. ErrorActionPreference='Continue'
#   ALWAYS. Never use 'Stop' with native 2>&1 (71-harvest-closeout G1).
#   Every native run uses Start-Process with separate stdout/stderr files.
# File encoding: UTF-8 with BOM + CRLF. ASCII content only.
param(
  [int]$Rounds = 30,
  [string]$CoreDir = '',
  [string]$VcBin = '',   # [85 B13] empty = auto-detect MSVC toolset; explicit path still honored
  [string]$AsanStage = 'C:\Users\Public\rbc-geom-gate',
  [string]$Baseline4KNoArg = '',
  [string]$Baseline4K16 = '',
  [string]$OutJson = ''
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($CoreDir -eq '') { $CoreDir = Join-Path $root 'src\core' }
$build = Join-Path $CoreDir 'build'
if ($OutJson -eq '') { $OutJson = Join-Path $build 'qa_geom_gate.json' }
$relExe  = Join-Path $build 'test_flushpar.exe'
$asanExe = Join-Path $build 'asan_test_flushpar.exe'
$asanPdb = Join-Path $build 'asan_test_flushpar.pdb'
$evidence = Join-Path $build 'qa_geom_gate_runs'
$failReasons = New-Object System.Collections.ArrayList
$cells = New-Object System.Collections.ArrayList
$illegal = New-Object System.Collections.ArrayList
$equival = New-Object System.Collections.ArrayList

function Add-Fail([string]$m) {
  [void]$failReasons.Add($m)
  Write-Output "[X] $m"
}
function Run-Native([string]$exe, [string[]]$argList, [string]$out, [string]$err) {
  $p = $null
  if ($null -eq $argList -or $argList.Count -eq 0) {
    $p = Start-Process -FilePath $exe -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput $out -RedirectStandardError $err
  } else {
    $p = Start-Process -FilePath $exe -ArgumentList $argList -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput $out -RedirectStandardError $err
  }
  if ($null -eq $p) { return -1 }
  return $p.ExitCode
}
function Read-Both([string]$out, [string]$err) {
  $s = ''
  if (Test-Path $out) { $v = Get-Content $out -Raw; if ($v) { $s += $v } }
  if (Test-Path $err) { $v = Get-Content $err -Raw; if ($v) { $s += $v } }
  return $s
}
function Get-NormLines([string]$path) {
  if (-not (Test-Path $path)) { return @() }
  $list = New-Object System.Collections.Generic.List[string]
  foreach ($l in (Get-Content $path)) {
    $s = $l -replace '\[empty-zone\]',''
    $s = $s -replace '\d+','#'
    $s = ($s -replace '\s+',' ').Trim()
    if ($s.Length -gt 0) { $list.Add($s) }
  }
  return $list.ToArray()
}
function Compare-Norm([string]$a, [string]$b, [string]$label) {
  if (-not (Test-Path $a) -or -not (Test-Path $b)) {
    Add-Fail "$label : baseline or output file missing"
    $bad = [ordered]@{ label=$label; lines_a=0; lines_b=0; mismatches=-1; verdict='FAIL' }
    return $bad
  }
  [string[]]$A = @(Get-NormLines $a)
  [string[]]$B = @(Get-NormLines $b)
  if ($A.Count -lt 2 -or $B.Count -lt 2) {
    Add-Fail "$label : normalized line count too small"
    $bad2 = [ordered]@{ label=$label; lines_a=$A.Count; lines_b=$B.Count; mismatches=-1; verdict='FAIL' }
    return $bad2
  }
  $n = [Math]::Min($A.Count, $B.Count)
  $d = 0
  for ($i = 0; $i -lt $n; $i++) { if ($A[$i] -cne $B[$i]) { $d++ } }
  $d += [Math]::Abs($A.Count - $B.Count)
  $r = [ordered]@{ label=$label; lines_a=$A.Count; lines_b=$B.Count; mismatches=$d; verdict='PASS' }
  if ($d -ne 0) { $r.verdict = 'FAIL'; Add-Fail "$label : $d structural mismatches" }
  return $r
}
function Invoke-Cell([int]$bs, [int]$lanes, [string]$mode, [string]$exe, [int]$rounds, [string]$tag) {
  $r = [ordered]@{
    block_size=$bs; lanes=$lanes; mode=$mode; rounds=$rounds;
    exit_codes=@(); exit_nonzero=0; fail_lines=0; bad_runs=0;
    asan_reports=0; first_bad_file=''; verdict='PASS'
  }
  $codes = New-Object System.Collections.ArrayList
  for ($i = 1; $i -le $rounds; $i++) {
    $stem = "gate_${tag}_bs${bs}_l${lanes}_r${i}"
    $out = Join-Path $evidence "$stem.out"
    $err = Join-Path $evidence "$stem.err"
    $argList = @("$lanes", '6', '20261106', "$bs")
    $rc = Run-Native $exe $argList $out $err
    [void]$codes.Add($rc)
    $txt = Read-Both $out $err
    $asan = 0
    if ($txt -match 'AddressSanitizer') {
      $asan = 1; $r.asan_reports++
      if (Test-Path $err) {
        Copy-Item $err (Join-Path $build "qa_geom_gate_ASAN_HIT_${tag}_bs${bs}_l${lanes}_r${i}.err") -Force
      }
    }
    $nf = ([regex]::Matches($txt, '\[FAIL\]')).Count
    $r.fail_lines += $nf
    if ($rc -ne 0) { $r.exit_nonzero++ }
    $bad = ($rc -ne 0) -or ($nf -gt 0) -or ($asan -eq 1) -or ($txt -match 'test_flushpar: FAIL')
    if ($bad) {
      $r.bad_runs++
      if ($r.first_bad_file -eq '') { $r.first_bad_file = $out }
      $r.verdict = 'FAIL'
      $r.exit_codes = @($codes.ToArray())
      Add-Fail "cell bs=$bs lanes=$lanes mode=$mode run=$i exit=$rc fail_lines=$nf asan=$asan output=$out"
      return $r
    }
  }
  $r.exit_codes = @($codes.ToArray())
  return $r
}

Write-Output '== qa_geom_gate: >4K concurrency gate (fail-closed) =='
Write-Output "rounds=$Rounds core=$CoreDir"

# --- fail-closed prerequisites ---
foreach ($p in @($relExe, $asanExe, $asanPdb)) {
  if (-not (Test-Path $p)) { Add-Fail "missing prerequisite: $p" }
}
if ($Rounds -lt 1) { Add-Fail 'Rounds must be >= 1' }
# [85 B13] Deterministic ASan toolset resolution (no hardcoded MSVC version).
# Root cause (84 report, two independent rounds): asan_*.exe imports
# clang_rt.asan_dynamic-x86_64.dll; each MSVC toolset ships a different build
# (14.44 = 1,193,496 B, 14.51 = 1,881,912 B). Default -VcBin was hardcoded to
# 14.44 while the staged exe was built with 14.51 -> loader fails with
# 0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND and 0-byte .out/.err = FALSE FAIL.
# Fix: when -VcBin is empty, enumerate installed MSVC toolset bins and pick the
# one whose ASan runtime actually loads the staged exe (single-run probe);
# an explicit -VcBin is still honored and verified the same way. The old
# "[warn] MSVC bin missing" is upgraded to fail-closed: in ASan mode a missing
# or mismatched bin now Add-Fail with a clear diagnostic instead of letting
# 360 runs produce a misleading false FAIL.
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
New-Item -ItemType Directory -Force -Path $AsanStage | Out-Null
$asanStaged = Join-Path $AsanStage 'asan_test_flushpar.exe'
Copy-Item $asanExe $asanStaged -Force
Copy-Item $asanPdb (Join-Path $AsanStage 'asan_test_flushpar.pdb') -Force

function Get-MsvcAsanBinCandidates {
  $roots = @(
    'C:\Program1\VC\Tools\MSVC',
    'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC',
    'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC',
    'C:\Program Files (x86)\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC'
  )
  $found = New-Object System.Collections.ArrayList
  foreach ($r in $roots) {
    if (-not (Test-Path $r)) { continue }
    foreach ($d in (Get-ChildItem -LiteralPath $r -Directory -ErrorAction SilentlyContinue)) {
      $bin = Join-Path $d.FullName 'bin\Hostx64\x64'
      if (Test-Path (Join-Path $bin 'clang_rt.asan_dynamic-x86_64.dll')) {
        [void]$found.Add($bin)
      }
    }
  }
  return $found.ToArray()
}

function Test-AsanRuntime([string]$bin, [string]$exe) {
  if (-not (Test-Path $bin)) { return $false }
  $saved = $env:PATH
  try {
    $env:PATH = "$bin;$env:SystemRoot\System32;$env:SystemRoot;$saved"
    $o = Join-Path $AsanStage 'vcprobe.out'
    $e = Join-Path $AsanStage 'vcprobe.err'
    $rc = Run-Native $exe @('4','6','20261106','8192') $o $e
    $txt = Read-Both $o $e
    Remove-Item $o,$e -Force -ErrorAction SilentlyContinue
    if (($rc -eq 0) -and ($txt -match 'RESULT: PASS') -and ($txt.Length -gt 0)) { return $true }
    return $false
  } finally { $env:PATH = $saved }
}

$effectiveVcBin = ''
if ($VcBin -ne '') {
  $effectiveVcBin = $VcBin
  if (-not (Test-Path $effectiveVcBin)) {
    Add-Fail "MSVC -VcBin path missing: $effectiveVcBin (ASan runtime cannot load; pass the toolset that built asan_*.exe, or leave default for auto-detect)"
  } elseif (-not (Test-AsanRuntime $effectiveVcBin $asanStaged)) {
    Add-Fail "-VcBin $effectiveVcBin does NOT match the ASan runtime of $asanExe (0xC0000139 false-FAIL risk); pass the toolset that built asan_*.exe or leave default"
  }
} else {
  $cands = @(Get-MsvcAsanBinCandidates)
  foreach ($c in $cands) {
    if (Test-AsanRuntime $c $asanStaged) { $effectiveVcBin = $c; break }
  }
  if ($effectiveVcBin -eq '') {
    Add-Fail "auto-detect failed: no installed MSVC toolset matched $asanExe (probed $($cands.Count) candidate bin(s)); rebuild asan_*.exe with one toolset or pass -VcBin"
  } else {
    Write-Output "[info] auto-detected ASan MSVC toolset bin: $effectiveVcBin"
  }
}
if ($effectiveVcBin -ne '') {
  $symbolizer = Join-Path $effectiveVcBin 'llvm-symbolizer.exe'
  $env:PATH = "$effectiveVcBin;$env:SystemRoot\System32;$env:SystemRoot;$env:PATH"
  $env:ASAN_SYMBOLIZER_PATH = $symbolizer
}

if ($failReasons.Count -gt 0) {
  # still emit JSON so CI has a machine-readable failure record
  $outObj = [ordered]@{ gate='qa_geom_gate'; verdict='FAIL'; fail_reasons=@($failReasons.ToArray()) }
  [System.IO.File]::WriteAllText($OutJson, ($outObj | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
  Write-Output 'GATE: FAIL'; exit 1
}

# --- 4K equivalence: no-arg 40 combos + single 16-lane run ---
$noargOut = Join-Path $build 'qa_geom_gate_4k_noarg.out'
$noargErr = Join-Path $build 'qa_geom_gate_4k_noarg.err'
$rcNoArg = Run-Native $relExe @() $noargOut $noargErr
$noargTxt = Read-Both $noargOut $noargErr
$eq1 = [ordered]@{
  label='4K no-arg sweep'; exit_code=$rcNoArg;
  runs40_bad0=($noargTxt -match 'runs=40 bad=0');
  result_pass=([regex]::Matches($noargTxt,'RESULT: PASS')).Count;
  fail_lines=([regex]::Matches($noargTxt,'\[FAIL\]')).Count;
  wb_gran_4096=([regex]::Matches($noargTxt,'wb_bytes%4096=0')).Count;
  verdict='PASS'
}
if (($rcNoArg -ne 0) -or (-not $eq1.runs40_bad0) -or ($eq1.result_pass -ne 40) -or
    ($eq1.fail_lines -ne 0) -or ($eq1.wb_gran_4096 -ne 40)) {
  $eq1.verdict='FAIL'; Add-Fail '4K no-arg sweep invariants failed'
}
[void]$equival.Add($eq1)

$l16Out = Join-Path $build 'qa_geom_gate_4k_l16.out'
$l16Err = Join-Path $build 'qa_geom_gate_4k_l16.err'
$rcL16 = Run-Native $relExe @('16','6','20261106') $l16Out $l16Err
$l16Txt = Read-Both $l16Out $l16Err
$eq2 = [ordered]@{
  label='4K lanes=16'; exit_code=$rcL16;
  runs8_bad0=($l16Txt -match 'runs=8 bad=0');
  result_pass=([regex]::Matches($l16Txt,'RESULT: PASS')).Count;
  fail_lines=([regex]::Matches($l16Txt,'\[FAIL\]')).Count;
  zones_266237=([regex]::Matches($l16Txt,'16 zones x 266237 B')).Count;
  active_zones_16=([regex]::Matches($l16Txt,'active_zones=16')).Count;
  set_exclude_16=([regex]::Matches($l16Txt,'set_exclude\(16 extents\)')).Count;
  verdict='PASS'
}
if (($rcL16 -ne 0) -or (-not $eq2.runs8_bad0) -or ($eq2.result_pass -ne 8) -or
    ($eq2.fail_lines -ne 0) -or ($eq2.zones_266237 -lt 1) -or
    ($eq2.active_zones_16 -ne 4) -or ($eq2.set_exclude_16 -ne 2)) {
  $eq2.verdict='FAIL'; Add-Fail '4K lanes=16 invariants failed'
}
[void]$equival.Add($eq2)

# optional structural diff against noon/engineer baselines (fail-closed if path given but absent)
if ($Baseline4KNoArg -ne '') {
  if (Test-Path $Baseline4KNoArg) { [void]$equival.Add((Compare-Norm $noargOut $Baseline4KNoArg '4K no-arg vs baseline')) }
  else { Add-Fail "Baseline4KNoArg not found: $Baseline4KNoArg" }
}
if ($Baseline4K16 -ne '') {
  if (Test-Path $Baseline4K16) { [void]$equival.Add((Compare-Norm $l16Out $Baseline4K16 '4K lanes=16 vs baseline')) }
  else { Add-Fail "Baseline4K16 not found: $Baseline4K16" }
}

# --- illegal geometry fail-closed ---
foreach ($v in @('4095','65536','131072','0','abc')) {
  $o = Join-Path $evidence "gate_illegal_$v.out"; $e = Join-Path $evidence "gate_illegal_$v.err"
  $rc = Run-Native $relExe @('16','6','20261106',$v) $o $e
  $t = Read-Both $o $e
  $r = [ordered]@{ block_size_arg=$v; exit_code=$rc; fail_closed=($t -match 'FAIL illegal block_size'); verdict='PASS' }
  if (($rc -ne 2) -or (-not $r.fail_closed)) { $r.verdict='FAIL'; Add-Fail "illegal block_size '$v' did not fail closed (exit=$rc)" }
  [void]$illegal.Add($r)
}

# --- >4K cells: Release + ASan, STOP-ON-FAIL ---
$bsList = @(8192,16384,32768)
$lanesList = @(4,16)
$stop = $false
foreach ($mode in @('Release','ASan')) {
  $exeToRun = $relExe
  if ($mode -eq 'ASan') { $exeToRun = $asanStaged }
  foreach ($bs in $bsList) {
    foreach ($lanes in $lanesList) {
      $tag = $mode.ToLower()
      $r = Invoke-Cell $bs $lanes $mode $exeToRun $Rounds $tag
      [void]$cells.Add($r)
      Write-Output ("  bs={0} lanes={1} mode={2}: bad={3}/{4} asan={5} exit_nonzero={6} {7}" -f `
        $bs,$lanes,$mode,$r.bad_runs,$Rounds,$r.asan_reports,$r.exit_nonzero,$r.verdict)
      if ($r.verdict -eq 'FAIL') { $stop = $true; break }
    }
    if ($stop) { break }
  }
  if ($stop) { break }
}

$verdict = 'PASS'
if ($failReasons.Count -gt 0) { $verdict = 'FAIL' }
$summary = [ordered]@{
  cells_run=$cells.Count; cells_failed=0; total_runs=0;
  asan_reports_total=0; illegal_failed=0; equival_failed=0;
  rounds_below_recommended=($Rounds -lt 30)
}
foreach ($c in $cells) { if ($c.verdict -eq 'FAIL') { $summary.cells_failed++ }; $summary.total_runs += $c.rounds; $summary.asan_reports_total += $c.asan_reports }
foreach ($r in $illegal) { if ($r.verdict -eq 'FAIL') { $summary.illegal_failed++ } }
foreach ($r in $equival) { if ($r.verdict -eq 'FAIL') { $summary.equival_failed++ } }

$outObj = [ordered]@{
  gate='qa_geom_gate'; timestamp=(Get-Date -Format 'o'); verdict=$verdict;
  rounds=$Rounds; stop_on_fail=$stop;
  release_exe=$relExe; asan_exe=$asanExe; asan_stage=$AsanStage;
  symbolizer=$symbolizer; symbolizer_available=(Test-Path $symbolizer);
  environment=[ordered]@{ powercfg_note='not asserted by this gate'; verifier_note='not asserted by this gate' };
  equivalence=@($equival.ToArray()); illegal=@($illegal.ToArray());
  cells=@($cells.ToArray()); summary=$summary;
  fail_reasons=@($failReasons.ToArray()); out_json=$OutJson
}
[System.IO.File]::WriteAllText($OutJson, ($outObj | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
Write-Output "JSON: $OutJson"
Write-Output "GATE: $verdict (cells=$($summary.cells_run) runs=$($summary.total_runs) asan_reports=$($summary.asan_reports_total))"
if ($verdict -eq 'PASS') { exit 0 } else { exit 1 }
