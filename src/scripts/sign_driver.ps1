# sign_driver.ps1 - sign the freshly built RAMBlockCache.sys, install it into
# dist\, write the sidecar, then re-sync the three deploy anchors.
#
# WHY (report 252, 2026-11-15): the A-batch patch (report 251) compiles and
# passes core/gates, but the deliverable is only "signable", not signed. Two
# hard constraints force this into a separate, elevated step:
#   1. signtool needs the LocalMachine\My private key  => must be elevated.
#   2. signtool plus PKCS#7 over a CJK path has failed on this host before
#      => sign in an ASCII stage directory, then copy back.
#
# Contract (all fail-closed):
#   * If signing, verification, or the Authenticode status check fails, dist\
#     is NOT touched.
#   * If the anchor sync fails AFTER dist\ was replaced, the previous binary
#     and sidecar are restored and the anchors are re-synced back, so the tree
#     never stays in a half-signed state (a half-signed tree makes deploy.ps1
#     fail closed and leaves the operator without a clear exit).
#   * No deploy, no service control, no registry write, no reboot. Deployment
#     stays a separate, explicitly approved step.
#
# Usage:
#   deliverables\RB-sign-driver.cmd              (self-elevating wrapper)
#   pwsh -File src\scripts\sign_driver.ps1 -Check   (read-only preflight)
#
# Exit: 0 = signed + anchors synced + read-back verified
#       1 = aborted (dist\ restored if it had been touched)
#       2 = aborted before any change (environment problem)

param(
    [string]$Sys = "",
    [string]$Thumbprint = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431',
    [string]$StageDir = 'C:\ProgramData\rb-deploy\sign-stage',
    [switch]$Check,
    [switch]$KeepStage
)

$ErrorActionPreference = 'Stop'

$script:LogLines = New-Object System.Collections.Generic.List[string]
$script:LogPath = $null
$script:DistTouched = $false
$script:BackupSys = $null
$script:BackupSidecar = $null
$script:DistSys = $null
$script:Sidecar = $null
$script:NewSha = $null
$script:OldSha = $null
$script:OldSidecarText = $null

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

function Resolve-SignTool {
    $c = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $pf = [Environment]::GetEnvironmentVariable('ProgramFiles')
    $roots = @()
    if ($pf86) { $roots += (Join-Path $pf86 'Windows Kits\10\bin') }
    if ($pf) { $roots += (Join-Path $pf 'Windows Kits\10\bin') }
    foreach ($r in $roots) {
        if (-not (Test-Path -LiteralPath $r)) { continue }
        $hit = Get-ChildItem -LiteralPath $r -Recurse -Filter 'signtool.exe' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

function Restore-Dist {
    # Put the previous binary + sidecar back and re-anchor to the old hash.
    if (-not $script:DistTouched) { return }
    Say 'rollback: restoring the previous dist\ binary and sidecar' 'warn'
    try {
        Copy-Item -LiteralPath $script:BackupSys -Destination $script:DistSys -Force
        if ($null -ne $script:OldSidecarText) {
            [IO.File]::WriteAllText($script:Sidecar, $script:OldSidecarText,
                (New-Object System.Text.ASCIIEncoding))
        } elseif (Test-Path -LiteralPath $script:Sidecar) {
            Remove-Item -LiteralPath $script:Sidecar -Force
        }
        $back = Get-Sha256Hex $script:DistSys
        if ($back -eq $script:OldSha) {
            Say ("rollback: dist\ restored, sha256=$back (matches the pre-sign hash)") 'ok'
        } else {
            Say ("rollback: dist\ restored but hash=$back != expected $($script:OldSha)") 'FAIL'
        }
        $sync = Join-Path $PSScriptRoot 'sync_anchors.ps1'
        $hostExe = (Get-Process -Id $PID).Path
        & $hostExe -NoProfile -ExecutionPolicy Bypass -File $sync -Sys $script:DistSys
        if ($LASTEXITCODE -eq 0) {
            Say 'rollback: anchors re-synced to the restored binary' 'ok'
        } else {
            Say "rollback: anchor re-sync FAILED (exit $LASTEXITCODE) - tree needs manual fixing" 'FAIL'
        }
    } catch {
        Say "rollback: FAILED - $($_.Exception.Message)" 'FAIL'
    }
}

function Abort([string]$Message, [int]$Code = 1) {
    Say $Message 'FAIL'
    if ($Code -ne 2) { Restore-Dist }
    Flush-Log
    Write-Host ''
    Write-Host '=================================================================='
    if ($Code -eq 2) {
        Write-Host '  ABORTED BEFORE ANY CHANGE - dist\ and anchors untouched'
    } else {
        Write-Host '  SIGNING ABORTED - see the rollback lines above'
    }
    Write-Host '=================================================================='
    exit $Code
}

# ------------------------------------------------------------------ preamble
$root = Find-RepoRoot $PSScriptRoot
if (-not $root) {
    Write-Host '[FAIL ] cannot locate the repo root (no build\build.ps1 above src\scripts)'
    exit 2
}

$logDir = 'C:\ProgramData\rb-deploy'
if (-not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$script:LogPath = Join-Path $logDir ('sign-driver-' + $stamp + '.log')

Write-Host '=================================================================='
Write-Host '  RAMBlockCache DRIVER SIGN + ANCHOR SYNC'
if ($Check) { Write-Host '  MODE: read-only preflight (nothing will be signed or copied)' }
else { Write-Host '  MODE: sign + install into dist\ + re-sync anchors (no deploy)' }
Write-Host '=================================================================='
Write-Host ''

Say "repo     : $root"
Say "log      : $($script:LogPath)"
Say "utc      : $((Get-Date).ToUniversalTime().ToString('o'))"

# ---------------------------------------------------------------- elevation
$isAdmin = $false
try {
    $isAdmin = (New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
} catch { }
Say ("elevated : {0}" -f $isAdmin)
if (-not $isAdmin -and -not $Check) {
    Abort 'not elevated - the LocalMachine\My private key is unreachable. Launch deliverables\RB-sign-driver.cmd instead.' 2
}

# ---------------------------------------------------------------- source sys
if (-not $Sys) { $Sys = Join-Path $root 'src\driver\x64\Release\RAMBlockCache.sys' }
if (-not (Test-Path -LiteralPath $Sys)) {
    Abort "source sys not found: $Sys (run: build\build.ps1 -Target driver)" 2
}
$srcSize = (Get-Item -LiteralPath $Sys).Length
$srcSha = Get-Sha256Hex $Sys
Say "source   : $Sys"
Say ("           {0:N0} B   sha256={1}" -f $srcSize, $srcSha)

# ------------------------------------------------------------------- signtool
$signtool = Resolve-SignTool
if (-not $signtool) { Abort 'signtool.exe not found (PATH / Windows Kits)' 2 }
Say "signtool : $signtool"

# ---------------------------------------------------------------- certificate
$cert = $null
try {
    $cert = Get-ChildItem Cert:\LocalMachine\My -ErrorAction Stop |
        Where-Object { $_.Thumbprint -eq $Thumbprint }
} catch {
    Say "cert store read failed: $($_.Exception.Message)" 'warn'
}
if (-not $cert) {
    Abort "signing cert not found in LocalMachine\My (thumbprint $Thumbprint)" 2
}
$certSubject = ($cert.Subject -replace '\s+', ' ')
Say "cert     : $certSubject"
Say ("           thumbprint=$($cert.Thumbprint)  NotAfter=$($cert.NotAfter)  HasPrivateKey=$($cert.HasPrivateKey)")
if (-not $cert.HasPrivateKey) {
    Abort 'cert present but HasPrivateKey=False - the private key ACL or CSP is not available to this account' 2
}

# -------------------------------------------------------------- current dist
$distDir = Join-Path $root 'dist'
$script:DistSys = Join-Path $distDir 'RAMBlockCache.sys'
$script:Sidecar = "$($script:DistSys).sha256"
if (Test-Path -LiteralPath $script:DistSys) {
    $script:OldSha = Get-Sha256Hex $script:DistSys
    Say "dist now : $($script:OldSha) ($((Get-Item -LiteralPath $script:DistSys).Length) B)"
} else {
    Say 'dist now : (absent)' 'warn'
}
if (Test-Path -LiteralPath $script:Sidecar) {
    $script:OldSidecarText = [IO.File]::ReadAllText($script:Sidecar)
    Say ("sidecar  : {0}" -f ($script:OldSidecarText.Trim()))
} else {
    Say 'sidecar  : (absent)' 'warn'
}

if ($srcSha -eq $script:OldSha) {
    Say 'source hash == dist hash: this binary is ALREADY the signed deliverable' 'ok'
    if ($Check) {
        Say 'preflight complete - nothing to do' 'ok'
        Flush-Log
        exit 0
    }
}

if ($Check) {
    Say 'preflight OK: cert usable, signtool present, stage dir writable?' 'ok'
    $probe = Join-Path $StageDir '.probe'
    try {
        if (-not (Test-Path -LiteralPath $StageDir)) {
            New-Item -ItemType Directory -Path $StageDir -Force | Out-Null
        }
        [IO.File]::WriteAllText($probe, 'ok')
        Remove-Item -LiteralPath $probe -Force
        Say "stage    : $StageDir (writable)" 'ok'
    } catch {
        Say "stage    : $StageDir NOT writable - $($_.Exception.Message)" 'FAIL'
        exit 2
    }
    Say 'preflight complete - nothing was signed, dist\ untouched' 'ok'
    Flush-Log
    exit 0
}

# --------------------------------------------------------------------- stage
if (Test-Path -LiteralPath $StageDir) {
    Remove-Item -LiteralPath $StageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $StageDir -Force | Out-Null
$staged = Join-Path $StageDir 'RAMBlockCache.sys'
Copy-Item -LiteralPath $Sys -Destination $staged -Force
Say "staged   : $staged"

# ---------------------------------------------------------------------- sign
Say "sign     : signtool sign /sm /s My /sha1 $Thumbprint /fd SHA256"
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$signOut = & $signtool sign /sm /s My /sha1 $Thumbprint /fd SHA256 $staged 2>&1
$signRc = $LASTEXITCODE
$ErrorActionPreference = $prevEap
foreach ($o in @($signOut)) { if ("$o".Trim()) { Say "  sign| $o" } }
if ($signRc -ne 0) { Abort "signtool sign FAILED (exit $signRc)" 1 }

# -------------------------------------------------------------------- verify
Say 'verify   : signtool verify /pa /v'
$ErrorActionPreference = 'Continue'
$verOut = & $signtool verify /pa /v $staged 2>&1
$verRc = $LASTEXITCODE
$ErrorActionPreference = $prevEap
foreach ($o in @($verOut)) { if ("$o".Trim()) { Say "  vrfy| $o" } }
if ($verRc -ne 0) { Abort "signtool verify FAILED (exit $verRc)" 1 }

$authStatus = 'SIGNTOOL-ONLY'
if (Get-Command Get-AuthenticodeSignature -ErrorAction SilentlyContinue) {
    try {
        $a = Get-AuthenticodeSignature -LiteralPath $staged
        $authStatus = "$($a.Status)"
        if ($a.Status -eq 'Valid') {
            Say ("authenti : Valid  signer={0}" -f ($a.SignerCertificate.Subject -replace '\s+', ' ')) 'ok'
        } else {
            Abort "Authenticode status=$($a.Status) on the staged binary" 1
        }
    } catch {
        Say "authenti : cmdlet check skipped - $($_.Exception.Message)" 'warn'
    }
} else {
    Say 'authenti : Get-AuthenticodeSignature unavailable; signtool /pa is the authoritative check' 'warn'
}

$signedSize = (Get-Item -LiteralPath $staged).Length
$signedSha = Get-Sha256Hex $staged
Say ("signed   : {0:N0} B  sha256={1}" -f $signedSize, $signedSha)

# ------------------------------------------------------------------ dist swap
$bakDir = Join-Path $logDir ('sign-backup-' + $stamp)
New-Item -ItemType Directory -Path $bakDir -Force | Out-Null
if (Test-Path -LiteralPath $script:DistSys) {
    $script:BackupSys = Join-Path $bakDir 'RAMBlockCache.sys'
    Copy-Item -LiteralPath $script:DistSys -Destination $script:BackupSys -Force
}
if (Test-Path -LiteralPath $script:Sidecar) {
    $script:BackupSidecar = Join-Path $bakDir 'RAMBlockCache.sys.sha256'
    Copy-Item -LiteralPath $script:Sidecar -Destination $script:BackupSidecar -Force
}
Say "backup   : $bakDir"

try {
    Copy-Item -LiteralPath $staged -Destination $script:DistSys -Force
    $script:DistTouched = $true
    [IO.File]::WriteAllText($script:Sidecar, "SHA256: $signedSha" + "`r`n",
        (New-Object System.Text.ASCIIEncoding))
    Say "dist     : replaced with the signed binary; sidecar written" 'ok'
} catch {
    Abort "could not install into dist\ - $($_.Exception.Message)" 1
}

# --------------------------------------------------------------- anchor sync
$sync = Join-Path $PSScriptRoot 'sync_anchors.ps1'
if (-not (Test-Path -LiteralPath $sync)) {
    Abort "sync_anchors.ps1 missing: $sync" 1
}
Say 'anchors  : running sync_anchors.ps1'
$hostExe = (Get-Process -Id $PID).Path
$ErrorActionPreference = 'Continue'
$syncOut = & $hostExe -NoProfile -ExecutionPolicy Bypass -File $sync -Sys $script:DistSys 2>&1
$syncRc = $LASTEXITCODE
$ErrorActionPreference = $prevEap
foreach ($o in @($syncOut)) { if ("$o".Trim()) { Say "  anch| $o" } }
if ($syncRc -ne 0) { Abort "sync_anchors.ps1 FAILED (exit $syncRc)" 1 }

# ------------------------------------------------------------- read-back gate
$anchorFiles = @(
    (Join-Path $root 'src\scripts\deploy.ps1'),
    (Join-Path $root 'src\scripts\verify_deploy_signature.ps1')
)
$bad = 0
foreach ($f in $anchorFiles) {
    if (-not (Test-Path -LiteralPath $f)) { Say "anchor missing: $f" 'FAIL'; $bad++; continue }
    $text = [IO.File]::ReadAllText($f)
    $m = [regex]::Match($text, 'DefaultExpectedSha256\s*=\s*''?([0-9a-fA-F]{64})''?')
    if (-not $m.Success) { Say "no DefaultExpectedSha256 in $f" 'FAIL'; $bad++; continue }
    $got = $m.Groups[1].Value.ToLower()
    if ($got -eq $signedSha) {
        Say ("anchor   : {0} = {1}" -f (Split-Path -Leaf $f), $got) 'ok'
    } else {
        Say ("anchor   : {0} = {1} != {2}" -f (Split-Path -Leaf $f), $got, $signedSha) 'FAIL'
        $bad++
    }
}
$sideText = [IO.File]::ReadAllText($script:Sidecar)
$sm = [regex]::Match($sideText, '([0-9a-fA-F]{64})')
if ($sm.Success -and $sm.Groups[1].Value.ToLower() -eq $signedSha) {
    Say ("anchor   : dist sidecar = {0}" -f $sm.Groups[1].Value.ToLower()) 'ok'
} else {
    Say 'anchor   : dist sidecar mismatch' 'FAIL'
    $bad++
}
$liveSha = Get-Sha256Hex $script:DistSys
if ($liveSha -eq $signedSha) {
    Say ("live     : dist\RAMBlockCache.sys = {0}" -f $liveSha) 'ok'
} else {
    Say ("live     : dist file hash {0} != signed {1}" -f $liveSha, $signedSha) 'FAIL'
    $bad++
}
if ($bad -ne 0) { Abort "read-back gate: $bad anchor(s) do not match the signed binary" 1 }

# deck: the deployed copy under System32 is intentionally NOT touched here.
$live = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
if (Test-Path -LiteralPath $live) {
    Say ("deployed : {0}" -f (Get-Sha256Hex $live)) 'info'
    Say '           (unchanged on purpose - deploy.ps1 is a separate approval)' 'info'
}

if (-not $KeepStage) {
    try { Remove-Item -LiteralPath $StageDir -Recurse -Force } catch { }
}

Flush-Log
Write-Host ''
Write-Host '=================================================================='
Write-Host '  SIGN + ANCHOR SYNC COMPLETE'
Write-Host ('  signed sha256 : ' + $signedSha)
Write-Host ('  size          : ' + $signedSize + ' B')
Write-Host ('  authenticode  : ' + $authStatus)
Write-Host ('  log           : ' + $script:LogPath)
Write-Host ''
Write-Host '  dist\ and the three anchors now point at the new binary.'
Write-Host '  The running driver and System32 copy are UNCHANGED.'
Write-Host '  Next (separate approval): verify_deploy_signature.ps1, then deploy.ps1'
Write-Host '=================================================================='
exit 0
