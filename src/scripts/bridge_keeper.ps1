# RB bridge keeper (runs as SYSTEM from scheduled task).
# PowerShell is Unicode-native -> Chinese paths are safe here (unlike .cmd).
#
# [FIX 2026-11-11] $web pointed at <repo>\web, which does not exist since the
# source tree was reorganised (the app now lives at src\ui\web). Push-Location
# to a missing directory does NOT throw -- it silently leaves the process in
# the previous cwd, so node was launched from %SystemRoot%\System32 every time
# and the bridge never listened. The keeper then slept 5s and retried forever,
# logging nothing about why. Hence: verify BOTH paths before the loop, and
# verify the cwd actually took effect instead of trusting Push-Location.

$root = 'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包'
$node = 'C:\Users\陈\.workbuddy\binaries\node\versions\node-v27.0.0-alpha.0.0.0-win-x64\node.exe'
$web  = Join-Path $root 'src\ui\web'
$logd = 'C:\ProgramData\rb-bridge'
$log  = Join-Path $logd 'keeper.log'

function W($m) { Add-Content -Path $log -Value ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

$entry = Join-Path $web 'bridge\server\index.ts'
W "keeper start; pid=$PID node=$([bool](Test-Path $node)) web=$([bool](Test-Path $entry)) path=$web"

if (-not (Test-Path -LiteralPath $node)) { W 'FATAL: node binary missing; keeper exiting'; exit 1 }
if (-not (Test-Path -LiteralPath $entry)) { W 'FATAL: bridge entry missing; keeper exiting'; exit 1 }

# [FIX 2026-11-11] Single-instance lock. Two keeper processes each tried to
# launch node and each appended to bridge-run.log; the second one died every
# 5s with "The process cannot access the file ... being used by another
# process" and could never start the bridge. A global mutex makes the second
# keeper exit immediately instead of fighting for the log.
$mutexName = 'Global\RAMBlockCacheBridgeKeeper'
$mutex = New-Object System.Threading.Mutex($false, $mutexName)
$haveLock = $false
try { $haveLock = $mutex.WaitOne(0, $false) } catch [System.Threading.AbandonedMutexException] { $haveLock = $true }
if (-not $haveLock) {
    W "another keeper owns $mutexName; this instance exits"
    exit 0
}
W "keeper lock acquired: $mutexName"

$runLog = Join-Path $logd 'bridge-run.log'

while ($true) {
    $busy = $false
    try {
        $busy = [bool](Get-NetTCPConnection -LocalPort 5199 -State Listen -ErrorAction SilentlyContinue)
    } catch { $busy = $false }
    if (-not $busy) {
        Push-Location -LiteralPath $web
        # Compare normalised full paths: Get-Location can differ in casing or
        # trailing separator from the literal we passed in.
        $here = [IO.Path]::GetFullPath((Get-Location).Path).TrimEnd('\')
        $want = [IO.Path]::GetFullPath($web).TrimEnd('\')
        if (-not $here.Equals($want, [StringComparison]::OrdinalIgnoreCase)) {
            # Should be unreachable after the preflight above; log it rather
            # than silently running the bridge from the wrong directory.
            W "FATAL: cwd is '$here', expected '$want'"
            Pop-Location
            exit 1
        }
        try {
            # Open the log with ReadWrite sharing so a stray second writer can
            # never wedge the bridge again; append-only, one write per line.
            & $node 'bridge/server/index.ts' 2>&1 |
                ForEach-Object {
                    try {
                        $fs = [IO.File]::Open($runLog, 'Append', 'Write', 'ReadWrite')
                        try {
                            $sw = New-Object IO.StreamWriter($fs)
                            $sw.WriteLine($_)
                            $sw.Flush()
                        } finally { $fs.Dispose() }
                    } catch { }
                }
            W "node exited code=$LASTEXITCODE"
        } catch {
            W ("node launch failed: " + $_.Exception.Message)
        }
        Pop-Location
    }
    Start-Sleep -Seconds 5
}
