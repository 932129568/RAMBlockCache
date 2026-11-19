#Requires -Version 5.1
<#
  RB-l2-p0c-test.ps1  --  P0-C: enable L2 on ONE volume, observe, auto-rollback on any FAIL.

  WHY THIS EXISTS
    Features\Flags = 0 means rbL2.c never executes. P0-A/P0-B/P1-C can never be
    exercised on a real machine until L2 is actually enabled. This script is the
    one controlled way to do that, and the one controlled way to undo it.

  SAFETY MODEL (read before editing)
    1. FAIL-CLOSED PREFLIGHT. Every precondition is checked; any doubt aborts
       with exit 2 and changes nothing.
    2. CRASH-SAFE MARKER. A state file is written BEFORE enabling. If this script
       is re-run and finds a leftover marker, the previous run died (BSOD or
       power loss). It rolls back FIRST and exits 3. It never resumes a test
       whose outcome is unknown.
    3. AUTO-ROLLBACK. Any FAIL during the window -> l2 disable -> clear
       L2Letter/L2SizeGB (omitting this breaks D: remount, real F: incident)
       -> delete container/index -> verify -> exit 1.
    4. NO SOURCE TOUCHING. This script never writes under src\ (mtime would make
       the signed driver look stale).

  EXIT CODES
    0  PASS  (and rolled back unless -KeepOnPass)
    1  FAIL  (rolled back)
    2  REFUSED at preflight (nothing changed)
    3  RECOVERED from a previous crashed run (rolled back; re-run to test)
    4  internal error (marker / tooling)
    5  UNCOVERED: L2 opened and stayed healthy, but no store ever executed.
       Stability evidence only -- NOT a caching PASS, NOT a failure.
#>
param(
    # TOPOLOGY (194 audit): the letter in `l2 enable <X:>` is the volume that HOSTS
    # the L2 container, NOT the volume being accelerated. rbfeat.c:801
    # RbFeatRefuseLetter refuses to accelerate the container volume (auto sweep,
    # manual attach and runtime attach all gated). Therefore the container must live
    # on a DIFFERENT, non-accelerated volume from the load target. Same-letter is a
    # self-referential topology: container IO still traverses this filter, and the
    # .idx truncate path (rbio.c:226-273) triggers flush + discard_all + RbL2DropAll.
    [string]$ContainerLetter = 'C',
    [string]$LoadLetter      = 'D',
    [int]   $SizeGB   = 2,
    [int]   $WindowMin = 10,
    [int]   $LoadMB    = 128,
    [switch]$KeepOnPass,
    [switch]$DryRun,
    [switch]$RollbackOnly,
    [switch]$SkipDeviceProbe,
    [switch]$SelfTestLoad,
    # A-plan forensics (195/196): read-only sampling only. Does NOT change any
    # verdict, exit code, rollback path or judgement criterion.
    [switch]$ProbeStats,
    # [C-PLAN / 203 P0-D] The pre-C driver rejects every block with
    # blk >= s_Slots, so OUTSIDE-the-window is a hard failure. After the C
    # driver is deployed (hash-addressed slots) the window no longer exists:
    # an out-of-window load is exactly what the fix must serve. -PostC switches
    # Write-LbaProbe's verdict to the post-C semantics. Default = old driver,
    # so this script stays correct on the currently deployed signed binary.
    [switch]$PostC,
    # [P5-B / 211 section 7] Decisive re-read experiment. Round 1 proved the
    # WRITE side (stores=3 on out-of-window blocks) but L2Hits stayed 0.
    # Two confounders: rbL2.c:978 rejects len>64KB probes, and shrink stored
    # only 3 of 2048 blocks. -P5B adds:
    #   A. read the LIVE container record headers to get the exact volume LBAs
    #   B. re-read the whole load file in 64KB aligned chunks
    #   C. one 64KB unbuffered read per stored LBA directly on the volume
    # All read-only, no driver change. Default OFF so the round-1 flow stays
    # reproducible for A/B comparison.
    [switch]$P5B,
    # [C-SLICE 2026-11-13 / 218 section 5] Stage D. Stage C issues N separate
    # 64KB reads, which all take the UNCHANGED single-block path in
    # RbL2ProbeFill -- it can never exercise the new slice code. Stage D issues
    # ONE read of 16 consecutive stored 64KB blocks (= 1MB): that is a >64KB
    # miss range, which the old gate rejected outright and the slice patch must
    # split into 16 pieces (expect L2Hits += 16, L2Miss += 0).
    # Requires -P5B (stage A supplies the block list) and runs BEFORE stage B
    # for the same core-all-hit-fast-path reason as stage C.
    [switch]$SliceTest
)

$ErrorActionPreference = 'Continue'   # native stderr must not abort the run

# ---------------------------------------------------------------- paths
$ScriptDir = $PSScriptRoot
if ([string]::IsNullOrEmpty($ScriptDir)) { $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$Repo = Split-Path -Parent $ScriptDir
$Ctl       = Join-Path $Repo 'src\ctl\rbfeatctl.exe'
$Observe   = Join-Path $ScriptDir 'RB-l2-observe.ps1'
$DeployDir = 'C:\ProgramData\rb-deploy'
$StatePath = Join-Path $DeployDir 'l2-p0c-state.json'
$Bytes     = [char]0x005C
$CL        = $ContainerLetter.ToUpperInvariant()   # hosts RAMBoostL2c.bin/.idx
$LL        = $LoadLetter.ToUpperInvariant()        # volume being accelerated
$VolRoot   = $CL + ':' + $Bytes
$LoadRoot  = $LL + ':' + $Bytes
$BinPath   = $VolRoot + 'RAMBoostL2c.bin'
$IdxPath   = $VolRoot + 'RAMBoostL2c.idx'
$LoadPath  = $LoadRoot + 'rbc-l2-load.bin'
$RegFeat   = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features'
$SysPath   = 'C:\Windows\System32\drivers\RAMBlockCache.sys'

$Stamp   = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
# [208 S1-6] probe CSV delta baseline + re-read mode (set by the post-store
# re-read stage). Initialised here so Write-Probe is safe at every sample.
$script:PrevProbe = @{ s = 0; h = 0; m = 0 }
$script:RereadMode = 'n/a'
# [C-SLICE 2026-11-13] stage D tool exit code (-1 = not run / skipped).
$script:SliceExit = -1
$script:SliceHitsDelta = -1
# [C-SLICE 2026-11-13 round 3] coreMiss delta of the same sample. Stage D is only
# VALID when this is >= 1: coreMiss +0 means rbio.c:534's inline all-hit fast path
# completed the read without entering the worker, so RbFeatOnMiss (the only caller
# of RbL2ProbeFill) never ran. In that case dL2h=0 says NOTHING about the driver.
$script:LastProbeDcm = -1
$script:SliceCoreMissDelta = -1
$script:PrevCoreMiss = -1
$script:SliceDisplaced = $false
$LogPath = Join-Path $DeployDir ('l2-p0c-' + $Stamp + '.log')
$script:ProbeCsv = Join-Path $DeployDir ('l2-probe-' + $Stamp + '.csv')
if ($ProbeStats) {
    try {
        Set-Content -LiteralPath $script:ProbeCsv -Value 'tag,flags,workers,coreHit,coreMiss,pref,prefOK,l2s,l2h,l2m,l2state,admitRejects,dL2s,dL2h,dL2m,readMode' -Encoding UTF8 -ErrorAction Stop
    } catch {
        Write-Host ('[WARN] probe csv not writable: ' + $_.Exception.Message)
    }
}

# ---------------------------------------------------------------- logging
function Say {
    param([string]$Text = '')
    $line = (Get-Date).ToUniversalTime().ToString('HH:mm:ss') + 'Z ' + $Text
    Write-Host $line
    try { Add-Content -LiteralPath $LogPath -Value $line -Encoding UTF8 -ErrorAction Stop } catch { }
}
function SayFail { param([string]$T) Say ('[FAIL] ' + $T) }
function SayPass { param([string]$T) Say ('[PASS] ' + $T) }
function SayWarn { param([string]$T) Say ('[WARN] ' + $T) }

# ---------------------------------------------------------------- helpers
function Get-RegDword {
    param([string]$Path, [string]$Name)
    try {
        $v = Get-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction Stop
        if ($null -eq $v) { return $null }
        return [int64]$v.$Name
    } catch { return $null }
}
function Get-Flags {
    $f = Get-RegDword -Path $RegFeat -Name 'Flags'
    if ($null -eq $f) { return $null }   # unknown must NOT masquerade as 0
    return $f
}
function FmtFlags {
    param($F)
    if ($null -eq $F) { return 'UNREADABLE' }
    return ('0x{0:X}' -f $F)
}
function Get-Sha256 {
    param([string]$Path)
    try { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant() }
    catch { return $null }
}
function Get-AnchorHash {
    param([string]$Path)
    try {
        $t = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
        $m = [regex]::Match($t, 'DefaultExpectedSha256\s*=\s*[''"]?([0-9A-Fa-f]{64})')
        if ($m.Success) { return $m.Groups[1].Value.ToLowerInvariant() }
        return $null
    } catch { return $null }
}
function Get-ServiceState {
    try {
        $s = Get-Service -Name 'RAMBlockCache' -ErrorAction Stop
        return [string]$s.Status
    } catch { return 'MISSING' }
}
function Get-BridgeHealth {
    try {
        $r = Invoke-RestMethod -Uri 'http://127.0.0.1:5199/health' -TimeoutSec 5 -ErrorAction Stop
        return $r
    } catch { return $null }
}
function Get-LogLines {
    $p = Join-Path 'C:\ProgramData\RAMBlockCache\logs' ('driver-' + (Get-Date).ToString('yyyyMMdd') + '.log')
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    try { return @(Get-Content -LiteralPath $p -ErrorAction Stop) } catch { return $null }
}
function Get-ErrorSince {
    # Timestamp-based, so log rotation/truncation cannot hide a new ERROR.
    # Returns $null when the log is unreadable (caller must treat as unknown).
    param([datetime]$SinceUtc)
    $lines = Get-LogLines
    if ($null -eq $lines) { return $null }
    $errs = New-Object System.Collections.ArrayList
    foreach ($ln in $lines) {
        $m = [regex]::Match([string]$ln, '^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z)')
        if (-not $m.Success) { continue }
        try {
            $t = [datetime]::Parse($m.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture,
                    ([Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal))
        } catch { continue }
        if ($t -gt $SinceUtc -and ([string]$ln) -match '(?<![0-9A-Za-z_])ERROR(?![0-9A-Za-z_])') {
            [void]$errs.Add([string]$ln)
        }
    }
    return ,$errs.ToArray()
}
function Get-L2Status {
    # returns @{ FlagsL2='on|off'; State=<int>; StateName=..; Stores=..; Hits=..; Miss=..; Raw=@() }
    $o = @(& $Ctl 'l2' 'status' 2>&1 | ForEach-Object { [string]$_ })
    $txt = ($o -join "`n")
    $r = @{ FlagsL2 = '?'; State = -1; StateName = '?'; Stores = -1; Hits = -1; Miss = -1; Raw = $o }
    $m = [regex]::Match($txt, 'flagsL2=(on|off)')
    if ($m.Success) { $r.FlagsL2 = $m.Groups[1].Value }
    $m = [regex]::Match($txt, 'L2State=(\d+)')
    if ($m.Success) { $r.State = [int]$m.Groups[1].Value }
    $m = [regex]::Match($txt, 'state\s*:\s*([a-z\-]+)')
    if ($m.Success) { $r.StateName = $m.Groups[1].Value }
    $m = [regex]::Match($txt, 'stores=(\d+)\s+hits=(\d+)\s+miss=(\d+)')
    if ($m.Success) {
        $r.Stores = [int64]$m.Groups[1].Value
        $r.Hits   = [int64]$m.Groups[2].Value
        $r.Miss   = [int64]$m.Groups[3].Value
    }
    return $r
}
function Get-VolStatusMap {
    # `rbfeatctl vols` -> @{ 'D'='accelerated'; 'C'='bypass' }
    # Returns $null (NOT an empty map) when the device cannot be read: non-elevated
    # rbfeatctl exits 3, and an empty map would make "is it attached?" silently pass.
    $o = @(& $Ctl 'vols' 2>&1 | ForEach-Object { [string]$_ })
    $txt = ($o -join "`n")
    if ($txt -notmatch 'volumes\s*:') { return $null }
    $map = @{}
    foreach ($ln in $o) {
        $m = [regex]::Match($ln, '^\s*([A-Za-z]):\s+(accelerated|bypass)')
        if ($m.Success) { $map[$m.Groups[1].Value.ToUpperInvariant()] = $m.Groups[2].Value }
    }
    if ($map.Count -eq 0) { return $null }
    return $map
}

# ---------------------------------------------------------------- A-plan forensics
# 195/196: the first live run produced stores=0 hits=0 miss=0. These read-only
# probes distinguish "core never saw the reads" from "L2 window rejected them".
# Nothing here touches a verdict, an exit code, or the rollback path.
function Get-StatsSnapshot {
    # rbfeatctl stats -json -> PSCustomObject, or $null when unreadable.
    # -json emits: v flags workers hits miss bh bm pref pref_ok wbq wbd l2s l2h l2m
    try {
        $o = @(& $Ctl 'stats' '-json' 2>&1 | ForEach-Object { [string]$_ })
        if ($LASTEXITCODE -ne 0) { return $null }
        $m = [regex]::Match(($o -join "`n"), '\{[^\r\n]*\}')
        if (-not $m.Success) { return $null }
        $j = $m.Value | ConvertFrom-Json -ErrorAction Stop
        if ($null -eq $j) { return $null }
        return $j
    } catch { return $null }
}

function Write-Probe {
    param([string]$Tag)
    if (-not $ProbeStats) { return }
    $s = Get-StatsSnapshot
    if ($null -eq $s) { Say ('  probe| ' + $Tag + ' stats=UNREADABLE'); return }
    # `stats -json` omits L2state / admitRejects (rbfeatctl.c:451-476 only prints
    # them in text mode). admitRejects quantifies "how many admissions were
    # refused" -- the key discriminator between the window gate and cold-block
    # filtering (rbfeat.c:886 Hits<2). Grab it from the text form.
    $l2state = '?'
    $admit = '?'
    try {
        $to = @(& $Ctl 'stats' 2>&1 | ForEach-Object { [string]$_ })
        $tt = ($to -join "`n")
        $mt = [regex]::Match($tt, 'L2state=(\d+)\s+thermalStops=(\d+)\s+admitRejects=(\d+)')
        if ($mt.Success) { $l2state = $mt.Groups[1].Value; $admit = $mt.Groups[3].Value }
    } catch { }
    # [208 S1-6] per-sample deltas make the store/hit attribution explicit
    # (a cumulative counter alone cannot show WHERE the hit happened).
    if ($null -eq $script:PrevProbe) { $script:PrevProbe = @{ s = 0; h = 0; m = 0 } }
    $ds = [int]$s.l2s - [int]$script:PrevProbe.s
    $dh = [int]$s.l2h - [int]$script:PrevProbe.h
    $dm = [int]$s.l2m - [int]$script:PrevProbe.m
    $script:PrevProbe = @{ s = [int]$s.l2s; h = [int]$s.l2h; m = [int]$s.l2m }
    # [C-SLICE 2026-11-13] expose this sample's L2Hits delta so stage D can assert
    # "one >64KB read produced exactly N hits" without re-parsing the CSV.
    $script:LastProbeDh = $dh
    # [C-SLICE round 3] ...and the coreMiss delta, which proves the read actually
    # MISSED in core and therefore went through RbFeatOnMiss.
    $curMiss = [int]$s.miss
    if ($script:PrevCoreMiss -ge 0) { $script:LastProbeDcm = $curMiss - $script:PrevCoreMiss }
    else { $script:LastProbeDcm = -1 }
    $script:PrevCoreMiss = $curMiss
    $rmode = [string]$script:RereadMode
    $line = '  probe| ' + $Tag +
            ' flags=0x' + [Convert]::ToString([int]$s.flags, 16).ToUpperInvariant() +
            ' workers=' + [string]$s.workers +
            ' coreHit=' + [string]$s.hits + ' coreMiss=' + [string]$s.miss +
            ' bh=' + [string]$s.bh + ' bm=' + [string]$s.bm +
            ' pref=' + [string]$s.pref + '/' + [string]$s.pref_ok +
            ' l2s=' + [string]$s.l2s + ' l2h=' + [string]$s.l2h + ' l2m=' + [string]$s.l2m +
            ' dL2s=' + $ds + ' dL2h=' + $dh + ' dL2m=' + $dm +
            ' l2state=' + $l2state + ' admitRejects=' + $admit + ' readMode=' + $rmode
    Say $line
    try {
        Add-Content -LiteralPath $script:ProbeCsv -Value (
            $Tag + ',' + $s.flags + ',' + $s.workers + ',' + $s.hits + ',' + $s.miss + ',' +
            $s.pref + ',' + $s.pref_ok + ',' + $s.l2s + ',' + $s.l2h + ',' + $s.l2m + ',' +
            $l2state + ',' + $admit + ',' + $ds + ',' + $dh + ',' + $dm + ',' + $rmode
        ) -Encoding UTF8 -ErrorAction Stop
    } catch { }
}

function Get-LoadLba {
    # fsutil file queryextents gives VCN/LCN per extent. LCN * cluster bytes =
    # volume byte offset, which is what rbL2.c uses for blk = lba / 64KB.
    if (-not (Test-Path -LiteralPath $LoadPath)) { return $null }
    $o = @(& fsutil file queryextents $LoadPath 2>&1 | ForEach-Object { [string]$_ })
    $lcns = @()
    foreach ($ln in $o) {
        $m = [regex]::Match($ln, 'LCN:\s*0x([0-9A-Fa-f]+)')
        if ($m.Success) { $lcns += [Convert]::ToInt64($m.Groups[1].Value, 16) }
    }
    if ($lcns.Count -eq 0) { return $null }
    $cluster = 4096L
    try {
        $v = Get-CimInstance Win32_Volume -Filter ("DriveLetter='" + $LL + ":'") -ErrorAction Stop
        if ($null -ne $v -and $v.BlockSize -gt 0) { $cluster = [int64]$v.BlockSize }
    } catch { }
    $minLcn = ($lcns | Measure-Object -Minimum).Minimum
    $maxLcn = ($lcns | Measure-Object -Maximum).Maximum
    return @{ Cluster = $cluster; Extents = $lcns.Count;
              MinLcn = $minLcn; MaxLcn = $maxLcn;
              MinByte = $minLcn * $cluster; MaxByte = ($maxLcn + 1) * $cluster;
              Lcns = $lcns }
}

function Write-LbaProbe {
    if (-not $ProbeStats) { return }
    $e = Get-LoadLba
    if ($null -eq $e) { SayWarn 'LBA probe: fsutil returned no extents (file absent or not NTFS)'; return }
    $slots = [int64][math]::Floor(($SizeGB * 1073741824L) / 65560L)
    $winBytes = $slots * 65536L
    $inWin = @($e.Lcns | Where-Object { ($_ * $e.Cluster) -lt $winBytes }).Count
    Say ('  lba| extents=' + $e.Extents + ' cluster=' + $e.Cluster +
         ' minLCN=0x' + [Convert]::ToString([int64]$e.MinLcn, 16).ToUpperInvariant() +
         ' maxLCN=0x' + [Convert]::ToString([int64]$e.MaxLcn, 16).ToUpperInvariant())
    Say ('  lba| volume bytes: min=' + $e.MinByte + ' (' + [math]::Round($e.MinByte / 1GB, 2) +
         ' GB)  max=' + $e.MaxByte + ' (' + [math]::Round($e.MaxByte / 1GB, 2) + ' GB)')
    Say ('  lba| slot capacity (C-plan): s_Slots=' + $slots + '  nominal bytes=' + $winBytes +
         ' (' + [math]::Round($winBytes / 1GB, 2) + ' GB)  blk_min=' +
         [int64][math]::Floor($e.MinByte / 65536L))
    Say ('  lba| extents in legacy prefix window: ' + $inWin + ' / ' + $e.Extents)
    if ($PostC) {
        # Post-C driver: container records live at hash slots, so the old
        # "prefix window" is gone. An out-of-window load is the regression
        # target -- it MUST be storable and probeable. This is observational
        # only; the real verdict comes from l2s>0 / L2Hits>0 in final checks.
        Say ('  lba| POST-C: legacy prefix window retired; blk_min > s_Slots is expected and must be served')
        # [210 review] This is STATIC GEOMETRY only (fsutil extents), NOT a
        # judgement-4 PASS. Printing SayPass here made users read it as proof
        # that out-of-prefix blocks were served. Use neutral Say + explicit
        # wording; the real evidence is l2s>0 AND l2h>0 in final checks.
        if ($inWin -eq 0) {
            Say 'LBA OBSERVATION (not a verdict): load lies entirely beyond the legacy prefix; judge J4 only from l2s>0 + L2Hits>0'
        } else {
            Say 'LBA OBSERVATION (not a verdict): load overlaps the legacy prefix; also exercise an out-of-prefix file (R1)'
        }
    } else {
        # Pre-C (currently deployed e3f6005d...): rbL2.c:976/1045 reject
        # every blk >= s_Slots ==> stores=0 is structurally guaranteed.
        if ($inWin -eq 0) {
            Say 'LBA OBSERVATION (not a verdict): load file lies entirely OUTSIDE the L2 window -- pre-C driver rejects every probe/store'
        } else {
            Say ('LBA OBSERVATION (not a verdict): ' + $inWin + ' extent(s) inside window -- window gate is NOT the sole blocker')
        }
    }
}

function Write-State {
    param([string]$Phase, [string]$Note = '')
    $o = [ordered]@{
        phase     = $Phase
        note      = $Note
        container = $CL
        load      = $LL
        sizeGB    = $SizeGB
        windowMin = $WindowMin
        utc       = (Get-Date).ToUniversalTime().ToString('o')
        pid       = $PID
        log       = $LogPath
    }
    try {
        New-Item -ItemType Directory -Path $DeployDir -Force -ErrorAction Stop | Out-Null
        $o | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $StatePath -Encoding UTF8 -ErrorAction Stop
        return $true
    } catch { return $false }
}
function Get-State {
    if (-not (Test-Path -LiteralPath $StatePath)) { return $null }
    # INVARIANT: if the marker file exists, this MUST return a non-null object.
    # PowerShell 7's ConvertFrom-Json does NOT throw on empty/whitespace input --
    # it emits no output at all, so `return (... | ConvertFrom-Json)` yields $null.
    # That made a 0-byte or truncated marker look like "no leftover state" and
    # let the run proceed into a NEW test with an unknown previous outcome.
    try {
        $raw = Get-Content -LiteralPath $StatePath -Raw -ErrorAction Stop
        if ([string]::IsNullOrWhiteSpace([string]$raw)) { return @{ phase = 'unreadable' } }
        $o = $raw | ConvertFrom-Json -ErrorAction Stop
        if ($null -eq $o) { return @{ phase = 'unreadable' } }
        return $o
    } catch { return @{ phase = 'unreadable' } }
}
function Remove-State {
    try { Remove-Item -LiteralPath $StatePath -Force -ErrorAction Stop } catch { }
}

function Initialize-UnbufferedReader {
    if (-not ([System.Management.Automation.PSTypeName]'Rbc.Native').Type) {
        Add-Type -Namespace Rbc -Name Native -ErrorAction Stop -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true, CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr CreateFileW(string name, uint access, uint share, System.IntPtr sec, uint disp, uint flags, System.IntPtr tmpl);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true)]
public static extern bool ReadFile(System.IntPtr h, System.IntPtr buf, uint n, out uint read, System.IntPtr ov);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true)]
public static extern bool CloseHandle(System.IntPtr h);
'@
    }
    return $true
}

function Invoke-UnbufferedReadPass {
    # FILE_FLAG_NO_BUFFERING: the Windows file cache sits ABOVE the filter driver.
    # A buffered read-back never reaches rbL2.c, so hits stay 0, admission is dropped
    # and no store ever happens. This is the only way to actually exercise the read path.
    param([string]$Path, [int]$ChunkKB = 1024, [long]$MaxBytes = 0)
    # 0x80000000 is INT32-negative in PowerShell and cannot convert to UInt32; use 2147483648.
    [uint32]$GENERIC_READ = 2147483648
    # [213 review item 6] READ|WRITE|DELETE (7), not 3. This helper only reads
    # the LOAD file today, but the container is opened by the driver with
    # FILE_SHARE_READ|WRITE|DELETE (rbL2.c:714); if this helper is ever pointed
    # at the container, share=3 would fail to open it.
    $SHARE_ALL     = 7
    $OPEN_EXISTING = 3
    $NO_BUFFERING  = 0x20000000
    $SEQUENTIAL    = 0x08000000
    $size = $ChunkKB * 1024
    $raw = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($size + 4096)
    try {
        $p = $raw.ToInt64()
        $aligned = [IntPtr](($p + 4095) -band (-bnot [int64]4095))
        $h = [Rbc.Native]::CreateFileW($Path, $GENERIC_READ, $SHARE_ALL, [IntPtr]::Zero, $OPEN_EXISTING,
                                       ($NO_BUFFERING -bor $SEQUENTIAL), [IntPtr]::Zero)
        if ($h.ToInt64() -eq -1) {
            throw ('CreateFileW failed err=' + [System.Runtime.InteropServices.Marshal]::GetLastWin32Error())
        }
        $total = 0L
        $acc = 0L
        $read = [uint32]0
        try {
            while ([Rbc.Native]::ReadFile($h, $aligned, [uint32]$size, [ref]$read, [IntPtr]::Zero)) {
                if ($read -le 0) { break }
                $total += $read
                $tmp = New-Object byte[] ([int]$read)
                [System.Runtime.InteropServices.Marshal]::Copy($aligned, $tmp, 0, [int]$read)
                for ($k = 0; $k -lt $read; $k += 4096) { $acc = ($acc + $tmp[$k]) % 1000003 }
                # [C-SLICE F1] Optional early stop: a SHORT range is what the warm-up
                # needs. Reading only a few MB guarantees the chunks stay resident in
                # core long enough to reach Hits>=2, which a 128 MB sweep cannot
                # guarantee when the dynamic watermark is below the file size.
                if ($MaxBytes -gt 0 -and $total -ge $MaxBytes) { break }
            }
        } finally { [void][Rbc.Native]::CloseHandle($h) }
        return @{ Total = $total; Checksum = $acc }
    } finally {
        [System.Runtime.InteropServices.Marshal]::FreeHGlobal($raw)
    }
}

function Invoke-Load {
    param([int]$MB)
    # Write once (buffered + FlushFileBuffers), then read THREE times unbuffered.
    # hits++ only on an actual cache hit (cache_read.c:54). Pass 1 = miss/fill
    # (hits 0), pass 2 = hit (hits 1), pass 3 = hit (hits 2). Admission rejects
    # anything below 2 (rbfeat.c:886 `if (Hits < 2) return`), so two passes are
    # structurally incapable of ever producing a store -- three is the minimum.
    $oneMB = 1MB
    $buf = New-Object byte[] $oneMB
    $rnd = New-Object System.Random 20261112
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $fs = [System.IO.File]::Create($LoadPath)
        try {
            for ($i = 0; $i -lt $MB; $i++) {
                $rnd.NextBytes($buf)
                $fs.Write($buf, 0, $buf.Length)
            }
            $fs.Flush($true)
        } finally { $fs.Close() }
        $wr = $sw.Elapsed.TotalSeconds
        Say ('  load: wrote ' + $MB + ' MB in ' + [math]::Round($wr,2) + 's')

        $unbuf = $true
        try { Initialize-UnbufferedReader | Out-Null } catch { $unbuf = $false; SayWarn ('Add-Type failed, falling back to buffered reads: ' + $_.Exception.Message) }

        for ($pass = 1; $pass -le 3; $pass++) {
            $sw.Restart()
            if ($unbuf) {
                try {
                    $r = Invoke-UnbufferedReadPass -Path $LoadPath
                } catch {
                    SayWarn ('unbuffered read failed (' + $_.Exception.Message + '), falling back to buffered')
                    $unbuf = $false
                    $r = $null
                }
            }
            if (-not $unbuf) {
                $out = [System.IO.File]::OpenRead($LoadPath)
                $total = 0L; $acc = 0L
                try {
                    while (($n = $out.Read($buf, 0, $buf.Length)) -gt 0) {
                        $total += $n
                        for ($k = 0; $k -lt $n; $k += 4096) { $acc = ($acc + $buf[$k]) % 1000003 }
                    }
                } finally { $out.Close() }
                $r = @{ Total = $total; Checksum = $acc }
            }
            $sec = $sw.Elapsed.TotalSeconds
            Say ('  read pass ' + $pass + ': ' + [math]::Round($r.Total/1MB,0) + ' MB in ' + [math]::Round($sec,2) + 's checksum=' + $r.Checksum + ' mode=' + $(if ($unbuf) { 'unbuffered' } else { 'BUFFERED(FALLBACK)' }))
            if ($r.Total -ne ($MB * $oneMB)) { SayWarn ('read pass ' + $pass + ' returned ' + $r.Total + ' bytes, expected ' + ($MB * $oneMB)) ; return $false }
        }
        $script:ReadPathUnbuffered = $unbuf
        return $true
    } catch {
        SayWarn ('load failed: ' + $_.Exception.Message)
        return $false
    }
}

function Invoke-Rollback {
    param([string]$Reason)
    Say ''
    Say '================ ROLLBACK ================'
    Say ('reason: ' + $Reason)
    Write-State -Phase 'rolling-back' -Note $Reason | Out-Null

    Say '[1] rbfeatctl l2 disable'
    $out = @(& $Ctl 'l2' 'disable' 2>&1 | ForEach-Object { [string]$_ })
    $rc = $LASTEXITCODE
    foreach ($ln in $out) { Say ('  ctl| ' + $ln) }
    Say ('  exit=' + $rc)
    Start-Sleep -Seconds 3

    # [208 review S1-5] Judgement 5: parse the FINALISED index in user mode.
    # Timing is critical: AFTER [1] l2 disable (which finalises via
    # CloseIntEx(Persist=TRUE)) but BEFORE [4] deletes $IdxPath.
    # Read-only, no driver change, no new IOCTL (203 section 1 P0-D).
    Say '[1b] parse finalised L2 index (judgement 5, user mode)'
    $Parser = Join-Path $Repo 'src\scripts\parse_l2_idx.py'
    if ((Test-Path -LiteralPath $IdxPath) -and (Test-Path -LiteralPath $Parser)) {
        $idxReport = Join-Path $DeployDir ('l2-idx-parse-' + $Stamp + '.txt')
        try {
            $po = @(& python $Parser $IdxPath 2>&1 | ForEach-Object { [string]$_ })
            foreach ($ln in $po) { Say ('  idx| ' + $ln) }
            try { Set-Content -LiteralPath $idxReport -Value $po -Encoding UTF8 -ErrorAction Stop } catch { }
            Say ('  idx report: ' + $idxReport)
        } catch { SayWarn ('idx parse failed: ' + $_.Exception.Message) }
    } else {
        SayWarn ('idx parse skipped (idx=' + (Test-Path -LiteralPath $IdxPath) +
                 ' parser=' + (Test-Path -LiteralPath $Parser) + ')')
    }

    Say '[2] verify Flags L2 bit cleared'
    $flags = Get-Flags
    if ($null -eq $flags) {
        SayFail 'Flags unreadable after rollback'
    } elseif ((($flags -band 4) -ne 0)) {
        SayFail ('Flags=' + (FmtFlags $flags) + ' still has L2 bit')
    } else {
        SayPass ('Flags=' + (FmtFlags $flags) + ' L2 bit clear')
    }

    Say '[3] verify L2Letter / L2SizeGB removed (else D: remount can be refused)'
    $letterLeft = Get-RegDword -Path $RegFeat -Name 'L2Letter'
    $gbLeft     = Get-RegDword -Path $RegFeat -Name 'L2SizeGB'
    if ($null -eq $letterLeft) { SayPass 'L2Letter absent' } else { SayFail ('L2Letter still = ' + $letterLeft) }
    if ($null -eq $gbLeft) { SayPass 'L2SizeGB absent' } else { SayFail ('L2SizeGB still = ' + $gbLeft) }

    Say '[4] delete container + index + load file'
    $cleanupFail = 0
    foreach ($p in @($BinPath, $IdxPath, $LoadPath)) {
        $done = $false
        for ($i = 1; $i -le 8; $i++) {
            if (-not (Test-Path -LiteralPath $p)) { $done = $true; break }
            try { Remove-Item -LiteralPath $p -Force -ErrorAction Stop } catch { }
            Start-Sleep -Seconds 1
        }
        if ($done) { SayPass ('absent: ' + $p) } else { SayFail ('could not delete: ' + $p); $cleanupFail++ }
    }

    Say '[5] post-rollback read-only observation'
    $obs = Invoke-Observe
    Say ('  observe exit=' + $obs)
    if ($obs -eq 1) { SayFail 'post-rollback observation has FAIL items' } else { SayPass 'post-rollback observation has no FAIL' }

    $state = Get-Flags
    # A failed file deletion must NOT be reported as ROLLBACK COMPLETE: leaving
    # a 2GB container or a stale index behind would make the next run's preflight
    # (and J11 stale-index risk) unreliable.
    if (($null -ne $state) -and ((($state -band 4) -eq 0)) -and ($null -eq (Get-RegDword -Path $RegFeat -Name 'L2Letter')) -and ($cleanupFail -eq 0)) {
        Write-State -Phase 'rolled-back' -Note $Reason | Out-Null
        Say 'ROLLBACK COMPLETE: L2 off, container/index/load deleted, L2Letter clean'
    } else {
        Write-State -Phase 'rollback-incomplete' -Note $Reason | Out-Null
        Say ('ROLLBACK INCOMPLETE: cleanupFail=' + $cleanupFail + ' -- manual attention required')
    }
    Say '=========================================='
    Say ''
}

function Invoke-Observe {
    $pwsh = 'C:\Program Files\PowerShell\7\pwsh.exe'
    if (-not (Test-Path -LiteralPath $pwsh)) { $pwsh = 'powershell.exe' }
    & $pwsh -NoProfile -ExecutionPolicy Bypass -File $Observe 2>&1 | ForEach-Object { Say ('  obs| ' + [string]$_) }
    return $LASTEXITCODE
}

function Get-DumpState {
    $files = @()
    foreach ($p in @('C:\Windows\MEMORY.DMP', 'C:\Windows\Minidump')) {
        if (Test-Path -LiteralPath $p) {
            try {
                $i = Get-Item -LiteralPath $p -ErrorAction Stop
                if ($i.PSIsContainer) {
                    foreach ($f in (Get-ChildItem -LiteralPath $p -File -ErrorAction SilentlyContinue)) {
                        $files += ($f.Name + '|' + $f.LastWriteTimeUtc.ToString('o'))
                    }
                } else {
                    $files += ($i.Name + '|' + $i.LastWriteTimeUtc.ToString('o'))
                }
            } catch { }
        }
    }
    return $files
}

# ================================================================ main
Say '=========================================================='
Say 'RAMBlockCache P0-C: enable L2 on one volume, observe, rollback'
Say ('log       : ' + $LogPath)
Say ('container : ' + $VolRoot + '  sizeGB=' + $SizeGB + '   <- L2 storage volume, must NOT be accelerated')
Say ('load      : ' + $LoadRoot + '  load=' + $LoadMB + 'MB   <- accelerated volume')
Say ('window    : ' + $WindowMin + 'min   dryRun=' + [bool]$DryRun + '   probeStats=' + [bool]$ProbeStats)
if ($ProbeStats) { Say ('probe csv  : ' + $script:ProbeCsv) }
Say '=========================================================='

foreach ($x in @($CL, $LL)) {
    if ($x.Length -ne 1 -or $x -lt 'A' -or $x -gt 'Z') { SayFail ('bad volume letter: ' + $x); exit 2 }
}
if ($CL -eq $LL) {
    SayFail ('self-referential topology: container and load are both ' + $CL + ': -- the container volume cannot be accelerated (RbFeatRefuseLetter). Use -ContainerLetter on a different volume.')
    exit 2
}
if (-not (Test-Path -LiteralPath $Ctl))     { SayFail ('missing ' + $Ctl);     exit 2 }
if (-not (Test-Path -LiteralPath $Observe)) { SayFail ('missing ' + $Observe); exit 2 }
if (-not (Test-Path -LiteralPath $VolRoot)) { SayFail ('container volume ' + $VolRoot + ' does not exist'); exit 2 }
if (-not (Test-Path -LiteralPath $LoadRoot)) { SayFail ('load volume ' + $LoadRoot + ' does not exist'); exit 2 }

# ---- load-path self-test (exercises Add-Type + unbuffered read, touches no L2 state)
if ($SelfTestLoad) {
    Say ('[mode] load self-test: write + 3 unbuffered read passes on ' + $LoadPath)
    $ok = Invoke-Load -MB 8
    $ub = $null
    if ($null -ne $script:ReadPathUnbuffered) { $ub = $script:ReadPathUnbuffered }
    Say ('unbuffered used: ' + [string]$ub)
    for ($i = 1; $i -le 5; $i++) {
        if (-not (Test-Path -LiteralPath $LoadPath)) { break }
        try { Remove-Item -LiteralPath $LoadPath -Force -ErrorAction Stop } catch { }
        Start-Sleep -Seconds 1
    }
    Say ('cleanup: ' + $LoadPath + ' absent=' + (-not (Test-Path -LiteralPath $LoadPath)))
    if ($ok -and $ub -eq $true) { Say 'SELFTEST: PASS'; exit 0 }
    Say 'SELFTEST: FAIL'
    exit 1
}

# ---- explicit rollback mode (used by RB-l2-p0c-rollback.cmd)
if ($RollbackOnly) {
    Say '[mode] rollback-only: turning L2 off and clearing all L2 state on this volume'
    $flags = Get-Flags
    if ($null -eq $flags) { SayWarn 'Flags unreadable; rolling back anyway'; $l2on = $true }
    else { $l2on = (($flags -band 4) -ne 0) }
    if (-not $l2on -and -not (Test-Path -LiteralPath $BinPath) -and -not (Test-Path -LiteralPath $IdxPath)) {
        Say 'nothing to roll back: L2 bit clear, no container/index present'
        Remove-State
        exit 0
    }
    Invoke-Rollback -Reason 'explicit rollback request'
    Remove-State
    $flags = Get-Flags
    if (($null -ne $flags) -and ((($flags -band 4) -eq 0)) -and ($null -eq (Get-RegDword -Path $RegFeat -Name 'L2Letter'))) { exit 0 }
    exit 1
}

# ---- crash recovery first
$prev = Get-State
if ($null -ne $prev) {
    $ph = [string]$prev.phase
    $prevUtc = [string]$prev.utc
    if ($prev.utc -is [datetime]) { $prevUtc = $prev.utc.ToUniversalTime().ToString('o') }
    Say ('[!] leftover state marker found: phase=' + $ph + ' utc=' + $prevUtc)
    if ($ph -eq 'pass-l2-on') {
        Say '[!] a previous run passed and left L2 ON; rolling back so this run starts clean'
    } else {
        Say '[!] previous run did not finish (crash/power loss). Rolling back before any new test.'
    }
    if ($DryRun) { Say 'dry run: would roll back now'; exit 0 }
    Invoke-Rollback -Reason ('leftover marker phase=' + $ph)
    Remove-State
    Say 'Recovered. Re-run this script to start a fresh test.'
    exit 3
}

# ---- preflight
Say ''
Say '--- preflight ---'
$bad = 0

$svc = Get-ServiceState
if ($svc -ne 'Running') { SayFail ('service=' + $svc); $bad++ } else { SayPass 'service Running' }

# Control device \\\\.\\RAMBlockCache has SDDL D:P(A;;GA;;SY)(A;;GA;;BA): only SYSTEM and
# Administrators may open it. Probe it BEFORE enabling -- otherwise a permission problem
# would surface only after L2 is on, and every status read would look like a FAIL.
$probe = Get-L2Status
$probeOk = $false
$probeSkipped = $false
if ($probe.State -lt 0) {
    if ($DryRun -and $SkipDeviceProbe) {
        SayWarn 'device probe skipped (-SkipDeviceProbe honored only with -DryRun)'
        $probeSkipped = $true
    } else {
        SayFail 'control device not openable: rbfeatctl l2 status failed (must run elevated)'
        foreach ($ln in $probe.Raw) { Say ('  ctl| ' + $ln) }
        $bad++
    }
} else {
    SayPass ('control device ok (L2State=' + $probe.State + ')')
    $probeOk = $true
}

if ($probeSkipped) {
    SayWarn 'real run cannot skip the device probe: elevation is mandatory for enable/disable'
}

$flags = Get-Flags
Say ('Flags=' + (FmtFlags $flags) + '  (PREFETCH=1 HOT=2 L2=4 WBCOAL=8)')
if ($null -eq $flags) {
    SayFail 'cannot read Features\Flags -- unknown must not be treated as 0'
    $bad++
} else {
    if ((($flags -band 4) -ne 0)) { SayFail 'L2 bit already set -- run RB-l2-p0c-rollback.cmd first'; $bad++ } else { SayPass 'L2 bit clear (0)' }
    if ((($flags -band 8) -ne 0)) { SayFail 'WBCOAL (0x8) set -- L2 and WBCOAL must not both be on'; $bad++ } else { SayPass 'WBCOAL (0x8) clear' }
}

$sysSha = Get-Sha256 -Path $SysPath
$a1 = Get-AnchorHash -Path (Join-Path $Repo 'src\scripts\deploy.ps1')
$a2 = Get-AnchorHash -Path (Join-Path $Repo 'src\scripts\verify_deploy_signature.ps1')
$side = $null
$sidePath = Join-Path $Repo 'dist\RAMBlockCache.sys.sha256'
if (Test-Path -LiteralPath $sidePath) {
    $t = (Get-Content -LiteralPath $sidePath -Raw -ErrorAction SilentlyContinue)
    $m = [regex]::Match([string]$t, '[0-9A-Fa-f]{64}')
    if ($m.Success) { $side = $m.Value.ToLowerInvariant() }
}
Say ('deployed sha = ' + $sysSha)
Say ('anchors      = ' + $a1 + ' / ' + $a2 + ' / ' + $side)
if ($null -eq $sysSha) { SayFail 'cannot hash deployed sys'; $bad++ }
elseif (($a1 -ne $sysSha) -or ($a2 -ne $sysSha) -or ($side -ne $sysSha)) { SayFail 'deployed sys does not match the three anchors'; $bad++ }
else { SayPass 'deployed sys == all three anchors' }

try {
    $sig = Get-AuthenticodeSignature -LiteralPath $SysPath -ErrorAction Stop
    if ([string]$sig.Status -eq 'Valid') { SayPass ('signature Valid (' + $sig.SignerCertificate.Subject + ')') }
    else { SayFail ('signature ' + [string]$sig.Status); $bad++ }
} catch { SayFail 'signature query failed'; $bad++ }

$h = Get-BridgeHealth
if ($null -eq $h) { SayWarn 'bridge /health unreachable (not fatal for the driver test)' }
elseif ($h.ok -ne $true) { SayWarn ('bridge ok=' + $h.ok) }
else { SayPass ('bridge health ok pid=' + $h.pid) }

# ---- topology gate (194 audit): the container host must NOT be the accelerated volume
if ($CL -eq $LL) {
    SayFail ('container letter == load letter (' + $CL + '): self-referential -- container IO would traverse the very filter that caches it')
    $bad++
} else {
    SayPass ('topology: container ' + $CL + ': != load ' + $LL + ':')
}

if ($probeOk) {
    $vols = Get-VolStatusMap
    if ($null -eq $vols) {
        SayFail 'rbfeatctl vols unreadable -- attach state unproven'; $bad++
    } else {
        $cs = $vols[$CL]; $ls = $vols[$LL]
        if ($ls -eq 'accelerated') { SayPass ($LL + ': accelerated (load target IS cached)') }
        else { SayFail ($LL + ': ' + $(if ($null -eq $ls) { 'absent from volstatus' } else { [string]$ls }) + ' -- load target MUST be accelerated'); $bad++ }
        if ($null -eq $cs -or $cs -eq 'bypass') { SayPass ($CL + ': ' + $(if ($null -eq $cs) { 'not in volstatus' } else { 'bypass' }) + ' (container host not accelerated)') }
        else { SayFail ($CL + ': accelerated -- container host MUST NOT be accelerated'); $bad++ }
    }
} else {
    SayWarn 'attach-state assertions skipped (device unreadable)'
}

if (-not (Test-Path -LiteralPath $VolRoot)) { SayFail ($VolRoot + ' does not exist'); $bad++ }
else {
    try {
        $di = New-Object System.IO.DriveInfo $CL
        $freeGB = [math]::Round($di.AvailableFreeSpace / 1GB, 1)
        Say ('free on ' + $VolRoot + ' (container) = ' + $freeGB + ' GB')
        if ($freeGB -lt ($SizeGB + 2)) { SayFail ('need at least ' + ($SizeGB + 2) + ' GB free for the container'); $bad++ } else { SayPass 'container volume free space sufficient' }
    } catch { SayFail ('cannot query ' + $VolRoot + ' geometry'); $bad++ }
}
if (-not (Test-Path -LiteralPath $LoadRoot)) { SayFail ($LoadRoot + ' does not exist'); $bad++ }
else {
    try {
        $di2 = New-Object System.IO.DriveInfo $LL
        $freeGB2 = [math]::Round($di2.AvailableFreeSpace / 1GB, 1)
        Say ('free on ' + $LoadRoot + ' (load) = ' + $freeGB2 + ' GB')
        if ($freeGB2 -lt (($LoadMB / 1024) + 1)) { SayFail ('need at least ' + [math]::Round(($LoadMB / 1024) + 1, 1) + ' GB free for the load file'); $bad++ } else { SayPass 'load volume free space sufficient' }
    } catch { SayFail ('cannot query ' + $LoadRoot + ' geometry'); $bad++ }
}

if (Test-Path -LiteralPath $BinPath) { SayFail ('container already exists: ' + $BinPath + ' (stale-state risk, remove manually)'); $bad++ } else { SayPass 'no pre-existing container' }
if (Test-Path -LiteralPath $IdxPath) { SayFail ('index already exists: ' + $IdxPath + ' (J11 stale-index risk, remove manually)'); $bad++ } else { SayPass 'no pre-existing index' }

$preDumps = Get-DumpState
Say ('pre-existing dump artifacts: ' + $preDumps.Count)
if ($preDumps.Count -gt 0) { foreach ($d in $preDumps) { Say ('  dump| ' + $d) } }

$preLog = Get-LogLines
if ($null -eq $preLog) { SayWarn 'today driver log missing; ERROR delta will be unavailable' ; $preLogCount = 0 }
else { $preLogCount = $preLog.Count; Say ('today driver log lines = ' + $preLogCount) }

Say ''
Say '--- baseline read-only observation (before enabling) ---'
$baseRc = Invoke-Observe
if ($baseRc -eq 1) { SayFail 'baseline observation has FAIL items -- fix those before enabling L2'; $bad++ }
elseif ($baseRc -eq 2) { SayWarn 'baseline observation has UNKNOWN items (allowed)' }
else { SayPass 'baseline observation clean' }

if ($bad -gt 0) {
    Say ''
    Say ('PREFLIGHT REFUSED: ' + $bad + ' blocking issue(s). Nothing was changed.')
    exit 2
}

if ($DryRun) {
    Say ''
    Say 'DRY RUN: preflight all green, would enable now. Exiting without changes.'
    exit 0
}

# ---- arm
if (-not (Write-State -Phase 'armed' -Note 'preflight green')) { SayFail 'cannot write state marker -- refusing to enable'; exit 4 }
Write-Probe 'baseline'

Say ''
Say '--- enable ---'
$out = @(& $Ctl 'l2' 'enable' $VolRoot $SizeGB 2>&1 | ForEach-Object { [string]$_ })
$rc = $LASTEXITCODE
foreach ($ln in $out) { Say ('  ctl| ' + $ln) }
Say ('  exit=' + $rc)
if ($rc -ne 0) {
    SayFail 'l2 enable returned non-zero'
    Invoke-Rollback -Reason 'enable failed'
    Remove-State
    exit 1
}

$flags = Get-Flags
if ($null -eq $flags) {
    SayFail 'cannot read Features\Flags after enable'
    Invoke-Rollback -Reason 'flag unreadable after enable'
    Remove-State
    exit 1
}
if ((($flags -band 4) -eq 0)) { SayFail 'Flags L2 bit not set after enable'; Invoke-Rollback -Reason 'flag not set'; Remove-State; exit 1 }
SayPass ('Flags=' + (FmtFlags $flags) + ' L2 bit set')
Write-State -Phase 'enabled' -Note 'L2 on, window starting' | Out-Null

$enabledAt = Get-Date
$enabledUtc = $enabledAt.ToUniversalTime()
Start-Sleep -Seconds 3

# ---- s_Halt guard (196 audit). After a previous disable, s_Halt deterministically
# stays at 1. `rbfeatctl.c:271` does not check Xfer and `rbL2.c:582` has a 300ms
# throttle, so the single REFRESH issued by `l2 enable` can be silently swallowed;
# Rb2Tick never calls LazyOpenInt, so the data path stays latched off forever with
# no self-healing. Send one supplementary REFRESH >=1s later, then require
# L2State==1 BEFORE starting the 10-minute window. state=2 is a self-healing
# transient (rbL2.c:667) so retry; 3/4 are hard refusals.
Say ''
Say '--- s_Halt guard: supplementary REFRESH + L2State==1 gate ---'
$rf = @(& $Ctl 'refresh' 2>&1 | ForEach-Object { [string]$_ })
$rfrc = $LASTEXITCODE
foreach ($ln in $rf) { Say ('  ctl| ' + $ln) }
Say ('  refresh exit=' + $rfrc)
Start-Sleep -Seconds 2
$openOk = $false
for ($i = 1; $i -le 5; $i++) {
    $st = Get-L2Status
    if ($st.State -eq 1) { $openOk = $true; SayPass ('L2State=1 (opened) on poll ' + $i); break }
    if ($st.State -eq 3 -or $st.State -eq 4) { SayFail ('L2State=' + $st.State + ' hard refusal after REFRESH'); break }
    SayWarn ('L2State=' + $st.State + ' on poll ' + $i + ' (transient? retrying)')
    Start-Sleep -Seconds 2
}
if (-not $openOk) {
    SayFail 'L2 never reached state=1 after supplementary REFRESH -- possible s_Halt latch (J17); reboot before retrying'
    Invoke-Rollback -Reason 'L2State never reached 1 after REFRESH'
    Remove-State
    exit 1
}
Say ('l2 status after enable: state=' + $st.StateName + '(' + $st.State + ') stores=' + $st.Stores)
Write-Probe 'post-enable'

# ---- load + window
Say ''
Say '--- load + observation window ---'
$loadOk = Invoke-Load -MB $LoadMB
if (-not $loadOk) { SayWarn 'load pass reported a problem (will still be judged by the checks below)' }
Write-Probe 'post-load'
Write-LbaProbe

# Store is only reachable through eviction (cache_evict -> rbfeat.c:886 hits>=2 -> rbL2.c:1022).
# Nothing evicts at 128MB against a GB-scale watermark, so force it explicitly.
Say ''
Say '--- forced eviction to exercise the store path ---'
Write-Probe 'pre-shrink'
$so = @(& $Ctl 'shrink' '100' 2>&1 | ForEach-Object { [string]$_ })
$src = $LASTEXITCODE
foreach ($ln in $so) { Say ('  ctl| ' + $ln) }
Say ('  shrink exit=' + $src)
if ($src -ne 0) { SayWarn 'shrink failed -- store path may stay unexercised (verdict will be UNCOVERED if so)' }
Start-Sleep -Seconds 3
Write-Probe 'post-shrink'

# [208 review S0-1] Store happens on eviction (shrink), but without a read
# AFTER the store, RbL2ProbeFill is never called and L2Hits stays 0
# STRUCTURALLY. One unbuffered pass drives core miss -> L2 probe.
# Must be AFTER shrink (the block must be in L2) and unbuffered (buffered
# reads never reach the filter driver, see :377-380).
Say ''
Say '--- post-store re-read (drives core miss -> L2 probe -> L2Hits) ---'
Write-Probe 'pre-reread'
$script:HitsAtPreReread = 0
try { $s0 = Get-StatsSnapshot; if ($s0) { $script:HitsAtPreReread = [int]$s0.l2h } } catch { }
# [P5-B / 211 section 7] Stage A: read LIVE container record headers.
# rbL2.c:712-716 opens the container with FILE_SHARE_READ|WRITE|DELETE and
# Store writes the whole record synchronously (:1099), so after shrink this
# is safe and gives the exact VOLUME-ABSOLUTE LBAs of the stored blocks.
# Pure read-only; all handles are closed by the Python tool's finally blocks.
$P5BJson = Join-Path $DeployDir ('p5b-blocks-' + $Stamp + '.json')
$P5BTool = Join-Path $Repo 'src\scripts\p5b_directed_reread.py'
$script:P5BBlocks = @()

# [C-SLICE 2026-11-13 / finding F1] Warm-up before stage A.
# The store path is reachable ONLY through eviction, and rbfeat.c:886 admits a
# block only when Hits >= 2. On a FRESH boot the file that was just written has
# been read only 3 times and the core cache does not hold all 128 MB, so the
# first `shrink 100` stores just a handful of scattered blocks. Live evidence:
# the driver log shows "l2: evict rejected (cold, hits<2)" at shrink time and
# L2Stores went 3 -> 3 instead of the 3 -> 2051 seen on a second pass inside the
# same boot. Stage D requires >= 16 CONSECUTIVE blocks, so re-read + shrink until
# the container actually holds a long enough run. Bounded to 3 rounds, read-only
# plus shrink, no driver change. If no run ever materialises, stage D reports
# itself as skipped (WARN) instead of pretending to pass.
if ($SliceTest -and $P5B -and (Test-Path -LiteralPath $P5BTool) -and (Test-Path -LiteralPath $BinPath)) {
    Say ''
    Say '--- C-SLICE warm-up: grow a consecutive run inside the container ---'
    $warmOk = $true
    try { Initialize-UnbufferedReader | Out-Null } catch { $warmOk = $false; SayWarn 'warm-up: Add-Type failed; skipping warm-up loop' }
    if ($warmOk) {
        $slotsArg = [string]([int][math]::Floor(($SizeGB * 1073741824L) / 65560L))
        for ($warm = 1; $warm -le 3; $warm++) {
            $wjson = Join-Path $DeployDir ('p5b-warm-' + $Stamp + '-' + $warm + '.json')
            $bestRun = 0
            $blkCount = 0
            try {
                $wargs = @($P5BTool, '--container', $BinPath, '--volume', $LL,
                           '--slots', $slotsArg, '--no-directed', '--json', $wjson)
                @(& python @wargs 2>&1) | Out-Null
                if (Test-Path -LiteralPath $wjson) {
                    $wb = @((Get-Content -LiteralPath $wjson -Raw | ConvertFrom-Json).blocks)
                    $bk = @($wb | ForEach-Object { [int]$_.blk } | Sort-Object -Unique)
                    $blkCount = $bk.Count
                    if ($blkCount -gt 0) {
                        $run = 1; $best = 1
                        for ($k = 1; $k -lt $blkCount; $k++) {
                            if ($bk[$k] -eq ($bk[$k - 1] + 1)) { $run++; if ($run -gt $best) { $best = $run } }
                            else { $run = 1 }
                        }
                        $bestRun = $best
                    }
                    Say ('  warm ' + $warm + ': blocks=' + $blkCount + ' longest consecutive run=' + $bestRun)
                }
            } catch { SayWarn ('warm-up scan failed: ' + $_.Exception.Message) }
            if ($bestRun -ge 16) { SayPass 'warm-up: container holds >=16 consecutive blocks'; break }
            if ($warm -ge 3) { SayWarn ('warm-up exhausted: best run=' + $bestRun + ' (<16) after 3 rounds'); break }
            # Admission needs Hits >= 2, i.e. the block must be MISSED once and then
            # HIT twice. `shrink 100` drives core down to ~1 MB, so every round starts
            # from an empty cache -- two passes would only reach Hits=1. Three is the
            # minimum that can ever produce a store (same reasoning as Invoke-Load).
            # Read only the FIRST 16 MB: 256 x 64 KB chunks. A short range stays
            # resident in core across all three passes (so Hits reaches 2) and, when
            # it does get evicted, it is evicted as one long run -- exactly what
            # stage D needs. Sweeping the whole 128 MB is what fails: the dynamic
            # watermark can sit below the file size, so later passes miss again and
            # Hits never reaches 2 (live evidence: "evict rejected (cold, hits<2)").
            for ($rp = 1; $rp -le 3; $rp++) {
                try { Invoke-UnbufferedReadPass -Path $LoadPath -ChunkKB 64 -MaxBytes (16MB) | Out-Null }
                catch { SayWarn ('warm-up re-read pass ' + $rp + ' failed: ' + $_.Exception.Message) }
            }
            @(& $Ctl 'shrink' '100' 2>&1) | Out-Null
            Say ('  warm ' + $warm + ': re-read x3 + shrink exit=' + $LASTEXITCODE)
            Start-Sleep -Seconds 2
        }
    }
}

if ($P5B) {
    Say ''
    Say '--- P5-B stage A: read live container record headers ---'
    # [213 review item 6] Precondition: without a store the container holds no
    # records and stage A would "find 0 blocks" -- that must NOT be read as a
    # driver defect. Gate on the live L2Stores counter first.
    $p5Pre = Get-L2Status
    if ($null -eq $p5Pre -or [int]$p5Pre.Stores -le 0) {
        SayWarn ('P5-B stage A skipped: L2Stores=' + $(if ($null -eq $p5Pre) { 'unreadable' } else { $p5Pre.Stores }) +
                 ' (no stored block exists yet; this is NOT a defect)')
    } elseif (-not (Test-Path -LiteralPath $P5BTool)) {
        SayWarn ('P5-B tool missing: ' + $P5BTool)
    } elseif (-not (Test-Path -LiteralPath $BinPath)) {
        SayWarn ('P5-B container missing although L2Stores=' + $p5Pre.Stores + ': ' + $BinPath)
    } else {
        try {
            $p5args = @($P5BTool, '--container', $BinPath, '--volume', $LL,
                        '--slots', [string]([int][math]::Floor(($SizeGB * 1073741824L) / 65560L)),
                        '--no-directed', '--json', $P5BJson)
            $p5out = @(& python @p5args 2>&1 | ForEach-Object { [string]$_ })
            foreach ($ln in $p5out) { Say ('  p5b| ' + $ln) }
            if ($LASTEXITCODE -ne 0) { SayWarn ('P5-B stage A tool exit=' + $LASTEXITCODE) }
            if (Test-Path -LiteralPath $P5BJson) {
                $script:P5BBlocks = @((Get-Content -LiteralPath $P5BJson -Raw | ConvertFrom-Json).blocks)
                Say ('  P5-B stored blocks = ' + $script:P5BBlocks.Count)
                $script:P5BOutOfPrefix = @($script:P5BBlocks | Where-Object { $_.out_of_legacy_prefix }).Count
                Say ('  P5-B out-of-legacy-prefix blocks = ' + $script:P5BOutOfPrefix)
                # [213 section 8] record-integrity evidence: if the container
                # record CRC verifies, a later MISS can only come from the
                # in-memory index, not from a torn/forged record.
                $crcRows = @((Get-Content -LiteralPath $P5BJson -Raw | ConvertFrom-Json).record_crc)
                if ($crcRows.Count -gt 0) {
                    $crcOk = @($crcRows | Where-Object { $_.crc_ok }).Count
                    Say ('  P5-B record CRC verified = ' + $crcOk + ' / ' + $crcRows.Count)
                }
            }
        } catch { SayWarn ('P5-B stage A failed: ' + $_.Exception.Message) }
    }
    Write-Probe 'post-p5b-stageA'
}

# [P5-B] Stage C runs BEFORE stage B on purpose (213 section 4.4 / section 9 #3).
# If it ran after the whole-file re-read, those 3 blocks would already be back
# in the core cache, and rbio.c:536 (inline all-hit fast path) would complete
# the read without ever calling RbFeatOnMiss -> RbL2ProbeFill. The probe would
# never execute and L2Hits would stay 0 for a reason that has nothing to do
# with L2: a false negative. Stage C must hit the blocks while they are still
# "miss in core, present in L2".
if ($P5B) {
    Say ''
    Say '--- P5-B stage C: directed 64KB volume reads at stored LBAs ---'
    Write-Probe 'pre-p5b-directed'
    if ($script:P5BBlocks.Count -eq 0) {
        SayWarn 'P5-B stage C skipped: no stored blocks from stage A'
    } elseif (-not (Test-Path -LiteralPath $P5BTool)) {
        SayWarn 'P5-B stage C skipped: tool missing'
    } else {
        try {
            $cargs = @($P5BTool, '--from-json', $P5BJson, '--volume', $LL)
            $cout = @(& python @cargs 2>&1 | ForEach-Object { [string]$_ })
            foreach ($ln in $cout) { Say ('  p5b| ' + $ln) }
            if ($LASTEXITCODE -ne 0) { SayWarn ('P5-B stage C tool exit=' + $LASTEXITCODE) }
        } catch { SayWarn ('P5-B stage C failed: ' + $_.Exception.Message) }
    }
    Write-Probe 'post-p5b-directed'
}

# [C-SLICE 2026-11-13] Stage D runs before stage B for the same reason stage C
# does: once the whole file is re-read into core, rbio.c:536's inline all-hit
# fast path never calls RbFeatOnMiss, so the new slice code would never run.
if ($SliceTest) {
    Say ''
    Say '--- C-SLICE stage D: ONE 1MB read over 16 consecutive stored blocks ---'
    if (-not $P5B) {
        SayWarn 'stage D skipped: -SliceTest needs -P5B (stage A supplies the block list)'
    } elseif ($script:P5BBlocks.Count -lt 16) {
        SayWarn ('stage D skipped: only ' + $script:P5BBlocks.Count + ' stored blocks (<16)')
    } elseif (-not (Test-Path -LiteralPath $P5BTool)) {
        SayWarn 'stage D skipped: tool missing'
    } else {
        # [C-SLICE round 5 / 2026-11-13] REAL root cause of four INCONCLUSIVE
        # rounds, proved by this run's CSV (l2-p0c-20261113T010151Z):
        #   rbio_worker.c:256 / :289 call rb_read_commit_gen() even when
        #   RbFeatOnMiss() returned SUCCESS (L2 already filled the buffer).
        #   cache_read.c:191 then does chunk_create + memcpy -> a stage-C L2
        #   HIT REPOPULATES core. The round-3 comment claiming "an L2 hit does
        #   not repopulate core" was WRONG.
        #   Stage D picks its 16 blocks from that exact repopulated set, so
        #   rb_read_try_cache() fully hits, rbio.c:534 completes the 1MB IRP
        #   inline, and RbFeatOnMiss (the ONLY caller of RbL2ProbeFill) never
        #   runs. Round 4 measured: the 64MB displacement's 1024 chunks were
        #   ALL core hits (coreHit +1023 / coreMiss +0) => it evicted NOTHING;
        #   the span then hit +1 / missed +0. Displacement cannot fix this.
        #
        # Fix: evict core with `shrink 100` IMMEDIATELY before the span, with NO
        # intervening read. This run proves shrink evicts exactly these blocks:
        # post-shrink showed l2s +2044 while coreHit/coreMiss stayed flat.
        # The displacement read is REMOVED -- any read afterwards refills core
        # and undoes the eviction. A probe pair brackets the eviction so the CSV
        # carries the l2s-delta evidence that it really happened.
        Write-Probe 'pre-cslice-evict'
        $displaced = $false
        try {
            $so = @(& $Ctl 'shrink' '100' 2>&1 | ForEach-Object { [string]$_ })
            $src = $LASTEXITCODE
            Say ('  slice| shrink(near-D) exit=' + $src)
            if ($src -ne 0) { SayWarn ('stage D: near-D shrink failed exit=' + $src) }
            $displaced = ($src -eq 0)
            # Eviction -> RbL2Store is a PASSIVE async job (RbEvictJob). Let it
            # settle so the span does not race a container write.
            Start-Sleep -Seconds 3
        } catch { SayWarn ('stage D: near-D shrink threw: ' + $_.Exception.Message) }
        Write-Probe 'post-cslice-evict'

        Write-Probe 'pre-cslice-span'
        try {
            $dargs = @($P5BTool, '--from-json', $P5BJson, '--volume', $LL, '--span-blocks', '16',
                       '--span-offset', '32')
            $dout = @(& python @dargs 2>&1 | ForEach-Object { [string]$_ })
            foreach ($ln in $dout) { Say ('  slice| ' + $ln) }
            $script:SliceExit = $LASTEXITCODE
            if ($LASTEXITCODE -ne 0) { SayWarn ('stage D tool exit=' + $LASTEXITCODE) }
        } catch { SayWarn ('stage D failed: ' + $_.Exception.Message) }
        Write-Probe 'post-cslice-span'
        $script:SliceHitsDelta = [int]$script:LastProbeDh
        $script:SliceCoreMissDelta = [int]$script:LastProbeDcm
        Say ('  stage D L2Hits delta across the 1MB span = ' + $script:SliceHitsDelta + ' (expect 16)')
        Say ('  stage D coreMiss delta across the 1MB span = ' + $script:SliceCoreMissDelta +
             ' (must be >=1: proves the read was a core MISS and reached RbFeatOnMiss)')
        $script:SliceDisplaced = $displaced
    }
}

# [P5-B] Stage B uses 64KB aligned chunks (round 1 used 1MB). rbL2.c:978
# rejects any probe with len > 64KB, so 64KB chunks are required to give the
# L2 read path a chance at all.
$rereadChunkKB = 1024
if ($P5B) { $rereadChunkKB = 64 }
$rereadMode = 'unknown'
try {
    Initialize-UnbufferedReader | Out-Null
    $rr = Invoke-UnbufferedReadPass -Path $LoadPath -ChunkKB $rereadChunkKB
    $rereadMode = 'unbuffered'
    $script:RereadMode = 'unbuffered'
    Say ('  re-read: ' + [math]::Round($rr.Total/1MB,0) + ' MB checksum=' + $rr.Checksum +
         ' mode=unbuffered chunkKB=' + $rereadChunkKB)
    # [208 S1-6 / 203 J3] INDIRECT evidence only: segmented checksum of the
    # re-read data vs the known written seed (weak 4096-byte-stride accumulator
    # from Invoke-UnbufferedReadPass). NOT a strict byte-for-byte comparison.
    Say ('  segmented checksum (indirect evidence): ' + $rr.Checksum +
         ' over ' + [math]::Round($rr.Total/1MB,0) + ' MB; strict byte-for-byte NOT implemented')
} catch {
    SayWarn ('unbuffered re-read failed (' + $_.Exception.Message + '), falling back to buffered')
    $rereadMode = 'buffered'
    $script:RereadMode = 'buffered'
    try {
        $acc = 0L; $tot = 0L
        $fsrr = [System.IO.File]::OpenRead($LoadPath)
        try {
            $buf2 = New-Object byte[] 1048576
            while (($nr = $fsrr.Read($buf2, 0, $buf2.Length)) -gt 0) {
                $tot += $nr
                for ($k = 0; $k -lt $nr; $k += 4096) { $acc = ($acc + $buf2[$k]) % 1000003 }
            }
        } finally { $fsrr.Close() }
        $rr = @{ Total = $tot; Checksum = $acc }
        Say ('  re-read: ' + [math]::Round($rr.Total/1MB,0) + ' MB checksum=' + $rr.Checksum + ' mode=buffered')
    } catch { SayWarn ('buffered re-read also failed: ' + $_.Exception.Message); $rr = $null }
}
Write-Probe 'post-reread'
if ($P5B) { Write-Probe 'post-reread-64k' }
if ($rereadMode -eq 'buffered') {
    SayWarn 'RE-READ was buffered: L2Hits=0 cannot distinguish "L2 broken" from "read never reached the filter driver"'
}

$fail = 0
$uncovered = $false
$state2Streak = 0
$deadline = $enabledAt.AddMinutes($WindowMin)
$lastState = -1
while ((Get-Date) -lt $deadline) {
    $svc = Get-ServiceState
    if ($svc -ne 'Running') { SayFail ('service left Running -> ' + $svc); $fail++; break }

    $st = Get-L2Status
    $lastState = $st.State
    if ($st.State -eq 4) { SayFail 'L2State=4 refused-config (container/params rejected)'; $fail++; break }
    if ($st.State -eq 3) { SayFail 'L2State=3 refused-blocksize'; $fail++; break }
    if ($st.State -eq 2) {
        # state 2 also covers "no volume attached yet" (rbL2.c:666) which self-heals;
        # requiring it to persist avoids a false FAIL on a transient.
        $state2Streak++
        SayWarn ('L2State=2 (no/multi volume) streak=' + $state2Streak)
        if ($state2Streak -ge 3) { SayFail 'L2State=2 persisted across 3 polls'; $fail++; break }
    } else { $state2Streak = 0 }

    $errs = Get-ErrorSince -SinceUtc $enabledUtc
    if ($null -eq $errs) {
        SayFail 'driver log unreadable during window: cannot prove ERROR==0'
        $fail++; break
    } elseif ($errs.Count -gt 0) {
        SayFail ('new ERROR lines in driver log: ' + $errs.Count)
        foreach ($e in ($errs | Select-Object -First 5)) { Say ('  log| ' + $e) }
        $fail++; break
    }

    $dumps = Get-DumpState
    if ($dumps.Count -gt $preDumps.Count) { SayFail 'new dump artifact appeared'; $fail++; break }

    Say ('  poll: state=' + $st.StateName + '(' + $st.State + ') stores=' + $st.Stores + ' hits=' + $st.Hits + ' miss=' + $st.Miss + ' svc=' + $svc)
    Write-Probe ('poll-' + (Get-Date).ToUniversalTime().ToString('HHmmss') + 'Z')
    Start-Sleep -Seconds 15
}

Say ''
Say '--- final checks ---'
Write-Probe 'final'
$st = Get-L2Status
Say ('final l2 status: flagsL2=' + $st.FlagsL2 + ' state=' + $st.StateName + '(' + $st.State + ') stores=' + $st.Stores + ' hits=' + $st.Hits + ' miss=' + $st.Miss)
if ($st.State -ne 1) { SayFail ('final L2State is ' + $st.State + ' (' + $st.StateName + '), expected 1 (opened)'); $fail++ }
else { SayPass 'L2 opened (state=1)' }
if ($st.Stores -gt 0) {
    SayPass ('L2Stores=' + $st.Stores + ' (store path executed)')
} elseif ($st.State -eq 1) {
    SayWarn ('L2Stores=0 while state=1: admission never admitted a block (hits<2 or watermark). Store path UNCOVERED, not PASS')
    $uncovered = $true
}

# [208 review S0-2] L2Stores>0 alone is NOT a caching PASS: 203 forbids it.
# Without this hard check, :945 prints VERDICT: PASS with l2s>0 and l2h=0
# (which is exactly the pre-C/first-run behaviour). $fail++ is mandatory.
# Use the increment across the post-store re-read when available, because
# `l2 status` hits is a cumulative counter since driver load.
$hitsDelta = [int]$st.Hits - [int]$script:HitsAtPreReread
Say ('  L2Hits delta over post-store re-read = ' + $hitsDelta +
     ' (final=' + $st.Hits + ', pre-reread=' + $script:HitsAtPreReread + ')')
if ($hitsDelta -gt 0) {
    SayPass ('L2Hits +' + $hitsDelta + ' after store: L2 probe hit path executed')
} elseif ($st.Hits -gt 0) {
    SayWarn ('L2Hits=' + $st.Hits + ' cumulative but no NEW hit during post-store re-read; treating as NOT proven')
    SayFail 'no L2Hits increment across post-store re-read: read-after-store NOT proven'
    $fail++
} else {
    SayFail ('L2Hits=0 while L2Stores=' + $st.Stores +
             ': store executed but no L2 probe ever hit -- read-after-store NOT proven')
    $fail++
}

$errs = Get-ErrorSince -SinceUtc $enabledUtc
if ($null -eq $errs) { SayFail 'driver log unreadable at final check: cannot prove ERROR==0'; $fail++ }
elseif ($errs.Count -gt 0) { SayFail ('ERROR lines accumulated: ' + $errs.Count); $fail++ }
else { SayPass 'zero ERROR lines since L2 was enabled' }

# [C-SLICE 2026-11-13] Stage D hard assertion: ONE 1MB read over 16 consecutive
# stored blocks must surface exactly 16 L2 hits. A delta of 0 means the >64KB
# range never reached RbL2FillSpan (old binary still loaded / gate not relaxed /
# blocks missing from the index) -- precisely the defect this round removes.
# Anything other than 0 or 16 is a broken assumption and is also a FAIL.
if ($SliceTest) {
    if ($script:SliceExit -ne 0) {
        SayWarn ('stage D not evaluated: tool exit=' + $script:SliceExit +
                 ' (layout/tool problem, NOT a driver verdict)')
    } elseif ($script:SliceCoreMissDelta -lt 1) {
        # [C-SLICE round 3] dL2h=0 with coreMiss=0 is INCONCLUSIVE, not a defect:
        # rbio.c:534 served the read inline from core, so RbFeatOnMiss was never
        # called and the slice code had no chance to run. Round 3 hit exactly this
        # (coreHit +1, coreMiss +0) even after the warm-up produced a 129-block run.
        # [round 5] If near-D shrink also yields coreMiss=0, this is a FIXTURE
        # limitation (core re-admission), still NOT a driver defect. Do not
        # weaken this gate to manufacture a PASS.
        SayWarn ('stage D INCONCLUSIVE: span read was a core HIT (coreMiss delta=' +
                 $script:SliceCoreMissDelta + ', L2Hits delta=' + $script:SliceHitsDelta +
                 ') -- rbio.c:534 completed it inline, RbFeatOnMiss never ran, so this ' +
                 'says NOTHING about the driver. core-evicted=' + $script:SliceDisplaced)
    } elseif ($script:SliceHitsDelta -eq 16) {
        SayPass 'stage D slice: ONE 1MB read (core MISS) produced L2Hits +16 -- 16 pieces served by RbL2FillSpan'
    } elseif ($script:SliceHitsDelta -eq 0) {
        SayFail 'stage D slice: ONE 1MB core-MISS read produced L2Hits +0 -- >64KB range went to disk, slice path did not serve it'
        $fail++
    } else {
        SayFail ('stage D slice: L2Hits delta=' + $script:SliceHitsDelta + ', expected exactly 16')
        $fail++
    }
}

$finalRc = Invoke-Observe
if ($finalRc -eq 1) { SayFail 'final observation has FAIL items'; $fail++ }
else { SayPass ('final observation rc=' + $finalRc) }

Say ''
if ($fail -gt 0) {
    Say ('VERDICT: FAIL (' + $fail + ' issue(s)) -- rolling back')
    Invoke-Rollback -Reason ('test FAIL, ' + $fail + ' issue(s)')
    Remove-State
    exit 1
}

if ($uncovered) {
    Say 'VERDICT: UNCOVERED -- L2 opened cleanly and the box stayed healthy, but the store'
    Say '         path never executed. This is neither PASS nor FAIL: it proves stability,'
    Say '         not that caching works. Report it as stability-only evidence.'
    Invoke-Rollback -Reason 'test UNCOVERED (store path not exercised)'
    Remove-State
    exit 5
}

# [208 P2-c] Do not let the fixture's own PASS be read as "caching works".
# The layered verdict (203 section 1 P0-D) is produced afterwards by
# src/scripts/p5_verdict.py from this log + CSV + idx report.
Say '  NOTE: this is a FIXTURE-level verdict. Layered judgement 1/2/4 + '
Say '        indirect 3/5 is produced separately by p5_verdict.py. '
Say '        J6 reboot-hit is NOT executed in v1; strict byte-for-byte is NOT implemented.'
Say 'VERDICT: FIXTURE-PASS (layered verdict pending)'
if ($KeepOnPass) {
    Write-State -Phase 'pass-l2-on' -Note 'left ON by -KeepOnPass' | Out-Null
    Say ('L2 left ON. Turn off later with: RB-l2-p0c-rollback.cmd')
} else {
    Say 'Rolling back (default policy: end the test at a known state)'
    Invoke-Rollback -Reason 'test PASS, default rollback'
    Remove-State
}
Say ('log: ' + $LogPath)
exit 0
