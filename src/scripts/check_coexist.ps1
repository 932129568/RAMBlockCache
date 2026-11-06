# check_coexist.ps1 -- read-only post-install health check for distribution machines.
# 2026-11-06 (U-9). Run in an ELEVATED PowerShell on the target machine AFTER
# installing, ideally with the third-party AV (e.g. 360) active. Pure ASCII
# output so it survives GBK/CP1252 consoles; no system changes, no writes.
# Exit 0 = no FAIL (WARN allowed); exit 1 = at least one FAIL.
param([string]$ExpectedSysSha256 = "")
$ErrorActionPreference = 'Continue'
$fail = 0; $warn = 0
function Ok([string]$m)   { Write-Host "[ok]   $m" }
function Warn([string]$m) { Write-Host "[warn] $m"; $script:warn++ }
function Bad([string]$m)  { Write-Host "[FAIL] $m"; $script:fail++ }

Write-Host "== check_coexist (read-only) =="
Write-Host ("  machine={0}  utc={1}" -f $env:COMPUTERNAME, (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))

# --- 1) service ---
$svc = Get-Service RAMBlockCache -ErrorAction SilentlyContinue
if (-not $svc) { Bad "service RAMBlockCache missing" }
else { Ok ("service: {0} / {1}" -f $svc.Status, $svc.StartType) }

# --- 2) signed driver + hash vs expected anchor ---
$sys = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
if (-not (Test-Path -LiteralPath $sys)) { Bad "System32 driver missing: $sys" }
else {
    $sig = Get-AuthenticodeSignature -LiteralPath $sys
    $hash = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash.ToLower()
    if ($sig.Status -ne 'Valid') { Bad ("driver signature = " + $sig.Status) }
    else { Ok ("driver signature Valid; sha256=" + $hash) }
    if (-not $ExpectedSysSha256) {
        $side = "$sys.sha256"
        if (Test-Path -LiteralPath $side) {
            $first = Get-Content -LiteralPath $side -TotalCount 1
            if ($first -match '(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])') { $ExpectedSysSha256 = $Matches[0].ToLower() }
        }
    }
    if ($ExpectedSysSha256 -and $hash -ne $ExpectedSysSha256.ToLower()) {
        Bad "System32 driver hash differs from expected anchor (running instance may still be the old build until reboot)"
    }
}

# --- 3) guard scripts ---
$gd = 'C:\ProgramData\RAMBoost'
foreach ($s in @('boot_guard.ps1','logon_disarm.ps1')) {
    if (Test-Path -LiteralPath (Join-Path $gd $s)) { Ok "guard script present: $s" }
    else { Warn "guard script MISSING: $s (boot_guard net degraded to signature gate + offline_rollback.cmd)" }
}

# --- 4) scheduled tasks ---
# Use the native cmdlet: schtasks.exe can be blocked by host policy, and a
# failed external command leaves $LASTEXITCODE stale -> false "task present".
# SYSTEM-level tasks may be invisible to a non-elevated / sandboxed token;
# report WARN (never a false OK) and tell the operator how to confirm.
foreach ($t in @('RAMBoostBootGuard','RAMBoostLogonDisarm','RAMBlockCacheBridge')) {
    $task = Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue
    if ($task) { Ok ("task present: {0} state={1}" -f $t, $task.State) }
    else { Warn "task NOT VISIBLE: $t (SYSTEM task may be hidden from this token, or AV blocked registration; confirm in your own elevated console with Get-ScheduledTask -TaskName '$t')" }
}

# --- 5) feature flags / write mode / watchman state ---
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Parameters'
if (Test-Path $par) {
    $p = Get-ItemProperty -Path $par
    Ok ("armed={0} healthy={1} bootfails={2} seq={3} ack={4}" -f $p.FilterArmed, $p.LastBootHealthy, $p.BootFailCount, $p.DeploySeq, $p.DeployAck)
    if ($p.WriteBack -eq 1) { Warn "WriteBack=1 (user explicitly enabled; ensure this is intended)" }
} else { Warn "Parameters key missing (fresh install or rolled back)" }
$feat = 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features'
if (Test-Path $feat) {
    $f = (Get-ItemProperty -Path $feat -Name Flags -ErrorAction SilentlyContinue).Flags
    if ($null -ne $f -and $f -ne 0) { Warn "Features\Flags=$f (default is 0 = all feature flags off)" }
    else { Ok "Features\Flags=0 (default)" }
}

# --- 6) AV interference hints (informational) ---
$av = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '360|QHSafe|ZhuDongFangYu' } | Select-Object -First 5 -ExpandProperty ProcessName
if ($av) { Warn ("AV processes present: " + ($av -join ',') + " - if boot_guard tasks are missing, this is the likely cause (installer degrades to 2-layer net)") }

# --- 7) Driver Verifier state ---
$mm = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management' -ErrorAction SilentlyContinue
if ($mm -and $mm.VerifyDrivers) { Warn ("Driver Verifier active: " + $mm.VerifyDrivers + " (expected during harvest cycle only)") }
else { Ok "Driver Verifier off" }

Write-Host ("== result: FAIL={0} WARN={1} ==" -f $fail, $warn)
if ($fail -gt 0) { exit 1 }
exit 0
