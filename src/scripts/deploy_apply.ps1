# deploy_apply.ps1 - gated wrapper around deploy.ps1 (the actual deployment).
#
# WHY (report 253, 2026-11-15): deploy.ps1 is correct and non-interactive, but
# it changes the boot driver, the service group, the volume-class UpperFilters
# and the boot-guard registry baseline, and it needs SYSRIGHT elevation. Two
# things are missing for a one-click run:
#   1. A pre-change gate I can read afterwards. deploy.ps1 verifies the source
#      binary, but the caller should also assert that dist\, the sidecar and the
#      two script anchors are the SAME binary before anything is touched.
#   2. A transcript. The elevated window closes and the evidence goes with it.
#      This wrapper logs every line to C:\ProgramData\rb-deploy\deploy-apply-<utc>.log
#      and prints a post-deploy state snapshot.
#
# Contract:
#   * Refuses to run if dist\ hash != any anchor, or if the signature is not
#     Valid. Nothing is changed in that case.
#   * Runs deploy.ps1 in a CHILD process: deploy.ps1 ends with exit, and calling
#     it with & in-process would terminate this wrapper before the snapshot.
#   * Never reboots. The reboot stays an explicit human action.
#   * No registry change of its own: every write below belongs to deploy.ps1.
#
# Usage:
#   deliverables\RB-deploy-driver.cmd      (self-elevating wrapper)
#   pwsh -File src\scripts\deploy_apply.ps1 -Check   (read-only gate only)
#
# Exit: 0 = deployed (reboot pending) / 1 = deploy failed and rolled back /
#       2 = refused before any change (gate / environment)

param(
    [string]$SysSource = '',
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

$script:LogLines = New-Object System.Collections.Generic.List[string]
$script:LogPath = $null

function Say([string]$Text, [string]$Tag = 'info') {
    $line = ('[{0,-5}] {1}' -f $Tag, $Text)
    Write-Host $line
    $script:LogLines.Add($line)
}

function Flush-Log {
    if (-not $script:LogPath) { return }
    try {
        [IO.File]::WriteAllLines($script:LogPath, $script:LogLines.ToArray(),
            (New-Object System.Text.UTF8Encoding($false)))
    } catch { }
}

function Get-Sha256Hex([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $fs = $null
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::ReadWrite)
        return ([BitConverter]::ToString($sha.ComputeHash($fs)) -replace '-', '').ToLower()
    } finally {
        if ($fs) { $fs.Dispose() }
        $sha.Dispose()
    }
}

function Find-RepoRoot([string]$Start) {
    $d = $Start
    for ($i = 0; $i -lt 8 -and $d; $i++) {
        if (Test-Path -LiteralPath (Join-Path $d 'build\build.ps1')) { return $d }
        $parent = Split-Path -Parent $d
        if (-not $parent -or $parent -eq $d) { break }
        $d = $parent
    }
    return $null
}

function Get-RegDword([string]$Path, [string]$Name) {
    try {
        $v = Get-ItemProperty -Path $Path -Name $Name -ErrorAction Stop
        if ($null -eq $v) { return $null }
        return $v.$Name
    } catch { return $null }
}

function Refuse([string]$Message) {
    Say $Message 'FAIL'
    Flush-Log
    Write-Host ''
    Write-Host '=================================================================='
    Write-Host '  REFUSED - nothing was changed'
    Write-Host '=================================================================='
    exit 2
}

# ------------------------------------------------------------------ preamble
$root = Find-RepoRoot $PSScriptRoot
if (-not $root) {
    Write-Host '[FAIL ] cannot locate the repo root (no build\build.ps1 above src\scripts)'
    exit 2
}
$logDir = 'C:\ProgramData\rb-deploy'
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$script:LogPath = Join-Path $logDir ('deploy-apply-' + $stamp + '.log')

Write-Host '=================================================================='
Write-Host '  RAMBlockCache DEPLOY (gated)'
if ($Check) { Write-Host '  MODE: gate only (nothing will be changed)' }
else { Write-Host '  MODE: verify -> deploy -> post-deploy snapshot (no reboot)' }
Write-Host '=================================================================='
Write-Host ''

Say "repo     : $root"
Say "log      : $($script:LogPath)"
Say "utc      : $((Get-Date).ToUniversalTime().ToString('o'))"

$isAdmin = $false
try {
    $isAdmin = (New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
} catch { }
Say ("elevated : {0}" -f $isAdmin)
if (-not $isAdmin -and -not $Check) {
    Refuse 'not elevated - launch deliverables\RB-deploy-driver.cmd instead'
}

# ------------------------------------------------------------------- gate
$distDir = Join-Path $root 'dist'
$distSys = Join-Path $distDir 'RAMBlockCache.sys'
$sidecar = "$distSys.sha256"
if (-not $SysSource) { $SysSource = $distSys }

Say '--- gate: the four sources must agree on one binary ---'
if (-not (Test-Path -LiteralPath $SysSource)) { Refuse "source missing: $SysSource" }
$distSha = Get-Sha256Hex $SysSource
Say ("dist     : {0}  ({1:N0} B)" -f $distSha, (Get-Item -LiteralPath $SysSource).Length)

$bad = 0
$anchors = @(
    (Join-Path $root 'src\scripts\deploy.ps1'),
    (Join-Path $root 'src\scripts\verify_deploy_signature.ps1')
)
foreach ($f in $anchors) {
    if (-not (Test-Path -LiteralPath $f)) { Say "anchor missing: $f" 'FAIL'; $bad++; continue }
    $t = [IO.File]::ReadAllText($f)
    $m = [regex]::Match($t, 'DefaultExpectedSha256\s*=\s*''?([0-9a-fA-F]{64})''?')
    if (-not $m.Success) { Say "no DefaultExpectedSha256 in $f" 'FAIL'; $bad++; continue }
    $got = $m.Groups[1].Value.ToLower()
    if ($got -eq $distSha) { Say ("anchor   : {0} = {1}" -f (Split-Path -Leaf $f), $got) 'ok' }
    else { Say ("anchor   : {0} = {1} != {2}" -f (Split-Path -Leaf $f), $got, $distSha) 'FAIL'; $bad++ }
}
if (Test-Path -LiteralPath $sidecar) {
    $st = [IO.File]::ReadAllText($sidecar)
    $sm = [regex]::Match($st, '([0-9a-fA-F]{64})')
    if ($sm.Success -and $sm.Groups[1].Value.ToLower() -eq $distSha) {
        Say ("anchor   : dist sidecar = {0}" -f $sm.Groups[1].Value.ToLower()) 'ok'
    } else { Say 'anchor   : dist sidecar mismatch' 'FAIL'; $bad++ }
} else {
    Say "sidecar  : missing ($sidecar)" 'FAIL'; $bad++
}
if ($bad -ne 0) { Refuse "gate failed: $bad anchor(s) disagree with the dist binary" }

$authStatus = 'UNAVAILABLE'
if (Get-Command Get-AuthenticodeSignature -ErrorAction SilentlyContinue) {
    try {
        $a = Get-AuthenticodeSignature -LiteralPath $SysSource
        $authStatus = "$($a.Status)"
        if ($a.Status -eq 'Valid') {
            Say ("signature: Valid  signer={0}" -f ($a.SignerCertificate.Subject -replace '\s+', ' ')) 'ok'
        } else {
            Say "signature: $($a.Status) - refusing to deploy an invalid binary" 'FAIL'
            Refuse 'signature gate failed'
        }
    } catch {
        Say "signature: cmdlet failed - $($_.Exception.Message)" 'FAIL'
        Refuse 'signature gate failed'
    }
} else {
    Say 'signature: Get-AuthenticodeSignature unavailable - deploy.ps1 will still enforce signtool /pa' 'warn'
}

# --------------------------------------------------------- current posture
Say '--- current posture (before) ---'
$clsKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'
$svcReg = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache'
$svcRoot = Join-Path $svcReg 'Parameters'
$featKey = Join-Path $svcReg 'Features'
$liveSys = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'

$liveSha = if (Test-Path -LiteralPath $liveSys) { Get-Sha256Hex $liveSys } else { 'ABSENT' }
Say "live sys : $liveSha"
Say ("           ({0:N0} B)" -f (Get-Item -LiteralPath $liveSys).Length)

$uf = $null
try { $uf = (Get-ItemProperty -Path $clsKey -Name UpperFilters -ErrorAction Stop).UpperFilters } catch { }
if ($uf) { Say ("UpperFil : {0}" -f ($uf -join ', ')) } else { Say 'UpperFil : (unreadable or absent)' 'warn' }

$svc = Get-Service -Name RAMBlockCache -ErrorAction SilentlyContinue
if ($svc) { Say ("service  : {0} / {1}" -f $svc.Status, $svc.StartType) } else { Say 'service  : NOT FOUND' 'warn' }

foreach ($n in @('DeploySeq', 'DeployAck', 'BootFailCount', 'LastBootHealthy', 'FilterArmed')) {
    $v = Get-RegDword $svcRoot $n
    Say ("{0,-15}: {1}" -f $n, $(if ($null -eq $v) { '(absent)' } else { $v }))
}
$flags = Get-RegDword $featKey 'Flags'
Say ("{0,-15}: {1}" -f 'Features\Flags', $(if ($null -eq $flags) { '(absent)' } else { ('0x{0:x}' -f [int]$flags) }))

Say '--- what deploy.ps1 will change ---'
Say '  System32\drivers\RAMBlockCache.sys  -> the signed dist binary (locked now => queued for next reboot)'
Say '  service RAMBlockCache              -> BOOT_START / System Bus Extender / ErrorControl=NORMAL'
Say '  volume-class UpperFilters          -> volsnap kept, RAMBlockCache re-appended'
Say '  boot baseline                      -> DeploySeq+1, DeployAck=0, BootFailCount=0, LastBootHealthy=0, FilterArmed=1'
Say '  TestSigned / Signer                -> stamped from the signing cert'
Say '  Features\Flags                     -> forced to 0 (feature flags off)'
Say '  guards + tasks                     -> boot_guard.ps1 / logon_disarm.ps1 reinstalled, 2 tasks re-registered'

if ($Check) {
    Say 'gate PASS - nothing was changed (read-only mode)' 'ok'
    Flush-Log
    Write-Host ''
    Write-Host '=================================================================='
    Write-Host '  GATE PASS - run without -Check to deploy'
    Write-Host '=================================================================='
    exit 0
}

# ---------------------------------------------------------------- run deploy
$deployPs1 = Join-Path $PSScriptRoot 'deploy.ps1'
if (-not (Test-Path -LiteralPath $deployPs1)) { Refuse "deploy.ps1 missing: $deployPs1" }
Say '--- running deploy.ps1 in a child process ---'
$hostExe = (Get-Process -Id $PID).Path
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$out = & $hostExe -NoProfile -ExecutionPolicy Bypass -File $deployPs1 -SysSource $SysSource 2>&1
$rc = $LASTEXITCODE
$ErrorActionPreference = $prevEap
foreach ($o in @($out)) { if ("$o".Trim()) { Say "$o" 'dep' } }
Say ("deploy.ps1 exit = {0}" -f $rc)

# ------------------------------------------------------- post-deploy snapshot
Say '--- post-deploy snapshot ---'
$liveSha2 = if (Test-Path -LiteralPath $liveSys) { Get-Sha256Hex $liveSys } else { 'ABSENT' }
Say "live sys : $liveSha2"
if ($liveSha2 -eq $distSha) {
    Say '           live copy already matches (deploy wrote through)' 'ok'
} else {
    Say '           live copy still the OLD binary - expected when the file was locked'
    $pfro = $null
    try { $pfro = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction Stop).PendingFileRenameOperations } catch { }
    if ($pfro) {
        $hit = @($pfro | Where-Object { "$_" -match 'RAMBlockCache\.sys' })
        if ($hit.Count -gt 0) {
            Say 'pending replace: PendingFileRenameOperations references RAMBlockCache.sys' 'ok'
            foreach ($h in $hit) { Say ("             {0}" -f $h) }
        } else {
            Say 'pending replace: NOT found in PendingFileRenameOperations' 'FAIL'
        }
    } else {
        Say 'pending replace: PendingFileRenameOperations unreadable/empty' 'FAIL'
    }
}

$uf2 = $null
try { $uf2 = (Get-ItemProperty -Path $clsKey -Name UpperFilters -ErrorAction Stop).UpperFilters } catch { }
if ($uf2) { Say ("UpperFil : {0}" -f ($uf2 -join ', ')) }
$svc2 = Get-Service -Name RAMBlockCache -ErrorAction SilentlyContinue
if ($svc2) { Say ("service  : {0} / {1}" -f $svc2.Status, $svc2.StartType) }
foreach ($n in @('DeploySeq', 'DeployAck', 'BootFailCount', 'LastBootHealthy', 'FilterArmed')) {
    $v = Get-RegDword $svcRoot $n
    Say ("{0,-15}: {1}" -f $n, $(if ($null -eq $v) { '(absent)' } else { $v }))
}
$flags2 = Get-RegDword $featKey 'Flags'
Say ("{0,-15}: {1}" -f 'Features\Flags', $(if ($null -eq $flags2) { '(absent)' } else { ('0x{0:x}' -f [int]$flags2) }))

Flush-Log
Write-Host ''
Write-Host '=================================================================='
if ($rc -eq 0) {
    Write-Host '  DEPLOY COMPLETE - reboot once to activate'
    Write-Host ''
    Write-Host '  After the reboot run: deliverables\RB-postreboot-verify.cmd'
    Write-Host '  Expected: PASS=8 FAIL=0, UpperFilters keeps RAMBlockCache, volumes > 0'
} else {
    Write-Host ('  DEPLOY FAILED (exit {0}) - deploy.ps1 ran its own rollback')
    Write-Host '  dist\ and the anchors were NOT touched by this wrapper.'
    Write-Host '  Read the log before retrying.'
}
Write-Host ('  log: ' + $script:LogPath)
Write-Host '=================================================================='
exit $rc
