#requires -Version 5.1
<#
.SYNOPSIS
  Write C:\ProgramData\rb-deploy\j6-manifest.json for the J6 reboot-hit verifier.

.DESCRIPTION
  [FIX G2 2026-11-15] This writer used to be an inline -Command string inside
  deliverables\RB-l2-j6-prepare.cmd and computed the deployed-image hash with
  Get-FileHash. On a host where that cmdlet does not resolve (same class of
  defect as report 244 / G1, which hit deploy.ps1), the fixture had already
  committed everything -- L2 bit ON, container C:\RAMBoostL2c.bin, index
  C:\RAMBoostL2c.idx, load D:\rbc-l2-load.bin, features\Flags=0x4 -- and then
  the LAST step died, leaving "J6 is NOT prepared" with L2 still ON. The fix
  is the same as G1: compute the hash with .NET SHA256 over a FileShare.ReadWrite
  handle, so no cmdlet and no share mode can block it; and keep Get-FileHash,
  when it does exist, only as a cross-check that can never make this script
  fail differently.

  Fail-closed rules kept from the inline version:
    * a fixture log older than THIS run (or than THIS boot) is refused,
    * a missing p5b-blocks-<stamp>.json is refused,
    * an unreadable boot time is refused,
    * the sys hash must be 64 lowercase hex chars.

.PARAMETER Repo
  Repository root. Default: two levels above this script (src\scripts\..\..).

.PARAMETER DeployDir
  Directory holding the fixture log / p5b report / manifest.
  Default: C:\ProgramData\rb-deploy

.PARAMETER FixtureExit
  Exit code of the fixture run being recorded. Recorded verbatim.

.PARAMETER StartUnix
  UTC epoch seconds captured BEFORE the fixture started. When > 0 the newest
  fixture log must be newer than this instant (proves the report is from THIS
  run, not a leftover p5b-blocks-*.json from 11-12/11-13). When 0 the weaker
  session rule applies: the log must be newer than the current boot time.

.PARAMETER LogPath
  Explicit fixture log. Default: newest l2-p0c-*.log in DeployDir.

.PARAMETER OutPath
  Explicit manifest path. Default: <DeployDir>\j6-manifest.json

.PARAMETER SysPath
  Deployed driver image to hash.
  Default: C:\Windows\System32\drivers\RAMBlockCache.sys

.OUTPUTS
  Exit 0 = manifest written. Exit 2 = refused (nothing written).

.NOTES
  READ-ONLY except for writing the single manifest file. No registry write,
  no service control, no volume IO, no reboot, no write-back / WBCOAL.
  Pure ASCII + UTF-8 BOM + CRLF (check_ps_syntax.ps1 contract), so it runs
  identically under Windows PowerShell 5.1 and pwsh 7.
#>
[CmdletBinding()]
param(
    [string]$Repo = '',
    [string]$DeployDir = 'C:\ProgramData\rb-deploy',
    [int]$FixtureExit = 0,
    [double]$StartUnix = 0,
    [string]$LogPath = '',
    [string]$OutPath = '',
    [string]$SysPath = 'C:\Windows\System32\drivers\RAMBlockCache.sys'
)

$ErrorActionPreference = 'Stop'

function Write-Fail([string]$Message, [int]$Code) {
    Write-Host ('[X] ' + $Message)
    exit $Code
}

function Write-Ok([string]$Message) {
    Write-Host ('[ok] ' + $Message)
}

# [FIX G2] Cmdlet-independent SHA256.
#   * .NET only: Get-FileHash does not exist on every host (report 244 / G1).
#   * FileShare.ReadWrite: a driver-held or PFRO-pending .sys raises a sharing
#     violation under the default FileShare.Read.
#   * Returns lowercase hex, or throws -- the caller fails closed.
function Get-Sha256Hex([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open,
                              [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $bytes = $sha.ComputeHash($fs)
        }
        finally {
            $fs.Dispose()
        }
    }
    finally {
        $sha.Dispose()
    }
    return (([BitConverter]::ToString($bytes)) -replace '-', '').ToLowerInvariant()
}

# Read the 32-byte RB2_L2IDX_H header with a share-mode that tolerates the
# driver holding the container open. Magic=0 here is EXPECTED (RbL2IdxUnseal
# zeroes it while the container is open); it is recorded, never judged.
function Read-IndexHeader([string]$Path) {
    $buf = New-Object byte[] 32
    $n = 0
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open,
                              [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $n = $fs.Read($buf, 0, 32)
        }
        finally {
            $fs.Dispose()
        }
    }
    catch {
        $n = 0
    }
    $h = [ordered]@{
        magic = '0x00000000'; magic_finalised = $false
        slot_count = 0; count = 0; boot_seq = [int64]0
        header_bytes = [int]$n
    }
    if ($n -ge 32) {
        $magic = [BitConverter]::ToUInt32($buf, 0)
        $h.magic = ('0x' + $magic.ToString('X8'))
        $h.magic_finalised = ($magic -eq 0x32494258)
        $h.slot_count = [int][BitConverter]::ToUInt32($buf, 8)
        $h.count = [int][BitConverter]::ToUInt32($buf, 12)
        $h.boot_seq = [int64][BitConverter]::ToUInt32($buf, 20)
    }
    return $h
}

$hostEngine = 'PowerShell ' + $PSVersionTable.PSVersion.ToString()
if ($PSVersionTable.PSEdition) { $hostEngine += ' / ' + $PSVersionTable.PSEdition }
Write-Host ('[..] host engine  : ' + $hostEngine)

if ([string]::IsNullOrWhiteSpace($Repo)) {
    # Walk up from src\scripts until a directory that owns src\ctl\rbfeatctl.exe
    # shows up. A fixed depth is wrong here: this file may be copied next to the
    # .cmd in future, and a silent off-by-one would point the manifest's ctl /
    # repo fields at "src" (which is exactly how G2 first failed its dry run).
    $probe = Split-Path -Parent $PSCommandPath
    for ($i = 0; $i -lt 4 -and $probe; $i++) {
        if (Test-Path -LiteralPath (Join-Path $probe 'src\ctl\rbfeatctl.exe')) {
            $Repo = $probe
            break
        }
        $probe = Split-Path -Parent $probe
    }
    if ([string]::IsNullOrWhiteSpace($Repo)) {
        Write-Fail ('cannot locate repo root above ' + $PSCommandPath) 2
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $Repo 'src\ctl\rbfeatctl.exe'))) {
    Write-Fail ('repo root does not contain src\ctl\rbfeatctl.exe: ' + $Repo) 2
}
if ([string]::IsNullOrWhiteSpace($OutPath)) {
    $OutPath = Join-Path $DeployDir 'j6-manifest.json'
}
if (-not (Test-Path -LiteralPath $DeployDir)) {
    Write-Fail ('deploy dir missing: ' + $DeployDir) 2
}

$epoch = [datetime]::new(1970, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
try {
    $bootUtc = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
}
catch {
    Write-Fail ('cannot read LastBootUpTime: ' + $_.Exception.Message) 2
}
if ($null -eq $bootUtc) {
    Write-Fail 'cannot read LastBootUpTime (null)' 2
}
Write-Host ('[..] baseline boot: ' + $bootUtc.ToString('o'))

# ---- 1) locate the fixture log and enforce freshness ----------------------
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $cand = @(Get-ChildItem -LiteralPath $DeployDir -Filter 'l2-p0c-*.log' -File |
              Sort-Object LastWriteTimeUtc)
    if ($cand.Count -eq 0) {
        Write-Fail ('no l2-p0c-*.log in ' + $DeployDir) 2
    }
    $log = $cand[$cand.Count - 1]
}
else {
    if (-not (Test-Path -LiteralPath $LogPath)) {
        Write-Fail ('fixture log missing: ' + $LogPath) 2
    }
    $log = Get-Item -LiteralPath $LogPath
}
if ($log.BaseName -notmatch '^l2-p0c-(.+)$') {
    Write-Fail ('unexpected fixture log name: ' + $log.Name) 2
}
$stamp = $Matches[1]
Write-Host ('[..] fixture log : ' + $log.FullName)

if ($StartUnix -gt 1700000000) {
    $cut = $epoch.AddSeconds($StartUnix).AddSeconds(-60)
    if ($log.LastWriteTimeUtc -lt $cut) {
        Write-Fail ('fixture log predates this run (log=' + $log.LastWriteTimeUtc.ToString('o') +
                    ' start=' + $cut.ToString('o') + ')') 2
    }
    Write-Host '[ok] freshness: log is newer than the captured start instant'
}
else {
    if ($log.LastWriteTimeUtc -lt $bootUtc) {
        Write-Fail ('fixture log predates this boot (log=' + $log.LastWriteTimeUtc.ToString('o') +
                    ' boot=' + $bootUtc.ToString('o') + ')') 2
    }
    Write-Host '[ok] freshness: log belongs to this boot session'
}

# ---- 2) stage-A report ---------------------------------------------------
$blocksPath = Join-Path $DeployDir ('p5b-blocks-' + $stamp + '.json')
if (-not (Test-Path -LiteralPath $blocksPath)) {
    Write-Fail ('missing stage-A report: ' + $blocksPath) 2
}
Write-Host ('[ok] stage-A report: ' + $blocksPath)

# ---- 3) deployed image hash (cmdlet independent) -------------------------
if (-not (Test-Path -LiteralPath $SysPath)) {
    Write-Fail ('deployed image missing: ' + $SysPath) 2
}
try {
    $sha = Get-Sha256Hex $SysPath
}
catch {
    Write-Fail ('cannot hash deployed image: ' + $_.Exception.Message) 2
}
if ($sha -notmatch '^[0-9a-f]{64}$') {
    Write-Fail ('bad sha256 for deployed image: ' + $sha) 2
}
Write-Host ('[ok] deployed image: ' + $sha)
if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
    try {
        $ref = (Get-FileHash -LiteralPath $SysPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($ref -ne $sha) {
            Write-Fail ('Get-FileHash cross-check disagrees (' + $ref + ' vs ' + $sha + ')') 2
        }
        Write-Host '[ok] cross-check: .NET SHA256 == Get-FileHash'
    }
    catch {
        Write-Host '[..] cross-check: Get-FileHash present but unusable; .NET value kept'
    }
}
else {
    Write-Host '[..] cross-check: Get-FileHash unavailable on this host; .NET value kept'
}

# ---- 4) prepare-time index header + repo parser --------------------------
$idxPath = 'C:\RAMBoostL2c.idx'
$head = Read-IndexHeader $idxPath
$head.note = ('Magic=0 at prepare is EXPECTED (RbL2IdxUnseal zeroes it while the ' +
              'container is open). Only a normal Start-menu reboot with clean ' +
              'shutdown H5 calls RbL2IdxSave and finalises it.')
Write-Host ('[..] index header : magic=' + $head.magic + ' boot_seq=' + $head.boot_seq +
            ' count=' + $head.count + ' bytes=' + $head.header_bytes)

$parser = Join-Path $Repo 'src\scripts\parse_l2_idx.py'
$parserExit = -1
$parserResult = ''
$parserOut = @()
if (-not (Test-Path -LiteralPath $parser)) {
    $parserOut = @('parse_l2_idx.py missing: ' + $parser)
}
elseif (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    $parserOut = @('python not found on PATH; header fields still recorded')
}
else {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $raw = @(& python $parser $idxPath 2>&1 | ForEach-Object { [string]$_ })
        $parserExit = $LASTEXITCODE
    }
    catch {
        $raw = @('parse_l2_idx.py not runnable: ' + $_.Exception.Message)
        $parserExit = -1
    }
    finally {
        $ErrorActionPreference = $prev
    }
    $parserOut = @($raw | Where-Object { $_ -notmatch '^\s+entry \d+:' })
    foreach ($line in $parserOut) {
        if ($line -match '^result:') { $parserResult = $line; break }
    }
}
$head.parser_exit = $parserExit
$head.parser_result = $parserResult
$head.parser_output = @($parserOut)
Write-Host ('[..] parse_l2_idx : exit=' + $parserExit + ' ' + $parserResult)

# ---- 5) manifest ---------------------------------------------------------
$manifest = [ordered]@{
    schema            = 'rbc-j6-manifest/1'
    created_utc       = (Get-Date).ToUniversalTime().ToString('o')
    host_engine       = $hostEngine
    baseline_boot_utc = $bootUtc.ToString('o')
    baseline_boot_unix = [math]::Round(($bootUtc - $epoch).TotalSeconds, 3)
    expect_sha        = $sha
    blocks_json       = $blocksPath
    fixture_log       = $log.FullName
    fixture_exit      = $FixtureExit
    fixture_switches  = '-ProbeStats -PostC -P5B -SliceTest -WindowMin 3 -KeepOnPass'
    repo              = $Repo
    ctl               = (Join-Path $Repo 'src\ctl\rbfeatctl.exe')
    volume            = 'D'
    container         = 'C:\RAMBoostL2c.bin'
    index             = $idxPath
    load              = 'D:\rbc-l2-load.bin'
    index_prepare     = $head
}
$json = $manifest | ConvertTo-Json -Depth 4
try {
    [IO.File]::WriteAllText($OutPath, $json, (New-Object Text.UTF8Encoding($false)))
}
catch {
    Write-Fail ('cannot write manifest (elevation needed?): ' + $_.Exception.Message) 2
}

Write-Ok ('manifest written: ' + $OutPath)
Write-Host ('[..] baseline_boot_utc = ' + $manifest.baseline_boot_utc)
Write-Host ('[..] expect_sha        = ' + $manifest.expect_sha)
Write-Host ('[..] blocks_json       = ' + $manifest.blocks_json)
Write-Host ('[..] index magic       = ' + $head.magic + ' boot_seq=' + $head.boot_seq + ' count=' + $head.count)
exit 0
