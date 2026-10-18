# verify_install.ps1 - 一键安装验收（只读，不改任何状态）
# 用法（提权窗）：powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\verify_install.ps1
# 覆盖：部署态/服务/过滤器/签名/testsigning/CLI 三通道/特性开关默认值

$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # 包根
$distSys = Join-Path $root 'dist\RAMBlockCache.sys'
$sysDest = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
$ctl     = Join-Path $root 'src\ctl\ramboostctl.exe'
$feat    = Join-Path $root 'src\ctl\rbfeatctl.exe'
$wpf     = Join-Path $root 'src\ui\controller\RAMBlockCache.Controller\bin\Release\net8.0-windows\RAMBlockCache.Controller.exe'

$pass = 0; $fail = 0
function Ok   ([string]$m) { Write-Host "[PASS] $m" -ForegroundColor Green;  $script:pass++ }
function Bad  ([string]$m) { Write-Host "[FAIL] $m" -ForegroundColor Red;    $script:fail++ }
function Info ([string]$m) { Write-Host "[..]   $m" }

Write-Host '== RAMBlockCache install verification ==' -ForegroundColor Cyan

# 0) 管理员
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if ($pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Ok 'elevated (Administrator)' }
else { Bad 'NOT elevated - rerun in an Administrator window (CLI/CLI IOCTL will be denied)'; }

# 1) 部署态：System32 副本 vs dist
if (-not (Test-Path -LiteralPath $sysDest)) { Bad "deployed sys missing: $sysDest" }
else {
    $hDeploy = (Get-FileHash -LiteralPath $sysDest -Algorithm SHA256).Hash.ToLower()
    $hDist   = (Get-FileHash -LiteralPath $distSys  -Algorithm SHA256).Hash.ToLower()
    Info "deployed sha256: $hDeploy"
    Info "dist     sha256: $hDist"
    if ($hDeploy -eq $hDist) { Ok 'deployed sys == dist sys (latest build)' }
    else { Bad 'deployed sys != dist sys - run deploy.ps1 then reboot (old build still loaded)' }
    $sig = (Get-AuthenticodeSignature -LiteralPath $sysDest).Status
    if ($sig -eq 'Valid') { Ok "deployed sys signature: $sig" } else { Bad "deployed sys signature: $sig" }
}

# 2) 服务
$svc = Get-Service RAMBlockCache -ErrorAction SilentlyContinue
if (-not $svc) { Bad 'service RAMBlockCache not found' }
else {
    Info "service: $($svc.Status) / $($svc.StartType)"
    if ($svc.Status -eq 'Running') { Ok 'service Running' } else { Bad "service status = $($svc.Status) (expect Running)" }
    if ($svc.StartType -eq 'Boot')  { Ok 'start type = Boot' } else { Bad "start type = $($svc.StartType) (expect Boot)" }
}

# 3) UpperFilters（volsnap 必须保留）——[FIX 建议-12] OpenSubKey 返回 null 时安全处理
$clsKey2 = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
    'SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}')
$uf = if ($clsKey2) { $clsKey2.GetValue('UpperFilters') } else { $null }
$ufStr = if ($uf) { [string]::Join(',', $uf) } else { '(key/value missing)' }
Info "UpperFilters: $ufStr"
if ($uf -contains 'volsnap') { Ok 'volsnap kept' } else { Bad 'volsnap missing from UpperFilters' }
if ($uf -contains 'RAMBlockCache') { Ok 'RAMBlockCache in UpperFilters' } else { Bad 'RAMBlockCache not in UpperFilters' }

# 4) testsigning
$ts = (& bcdedit /enum '{current}' 2>$null | Select-String 'testsigning') -join ' '
if ($ts -match 'Yes') { Ok 'testsigning: Yes' } else { Bad "testsigning not Yes ($ts) - self-signed driver will not load at boot" }

# 5) CLI 三通道（rb 通道 / RB2 通道 / 配置回读）
function RunCli([string]$exe, [string]$argsLine) {
    if (-not (Test-Path -LiteralPath $exe)) { Bad "tool missing: $exe"; return $null }
    $out = & $exe $argsLine.Split(' ') 2>&1 | Out-String
    return @{ out = $out; code = $LASTEXITCODE }
}
$r1 = RunCli $feat 'stats'
if ($r1 -and $r1.out -match 'hits=') { Ok 'rbfeatctl stats -> real data (RB2 channel OK)' } else { Bad "rbfeatctl stats failed: $($r1.out)" }
$r2 = RunCli $feat 'vols'
if ($r2 -and $r2.out -match 'bypass|attached') { Ok 'rbfeatctl vols -> volume list OK' } else { Bad "rbfeatctl vols failed: $($r2.out)" }
$r3 = RunCli $ctl 'get-config'
if ($r3 -and $r3.out -match '4K|块|Block') { Ok 'ramboostctl get-config -> config readback OK' } else { Bad "ramboostctl get-config failed: $($r3.out)" }

# 6) 特性开关默认值（红线：Features\Flags 全关）
$flags = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\RAMBlockCache\Features' -ErrorAction SilentlyContinue).Flags
if ($null -eq $flags) { Info 'Features\Flags not present (driver defaults to 0 = all off)'; Ok 'flags default off' }
elseif ($flags -eq 0) { Ok 'Features\Flags = 0 (all features off, safe default)' }
else { Bad ("Features\Flags = 0x{0:X} (expect 0 unless intentionally enabled)" -f $flags) }

# 7) WPF 可执行性
if (Test-Path -LiteralPath $wpf) { Ok "WPF exe present: $wpf" } else { Bad "WPF exe missing: $wpf" }

# 汇总
Write-Host ''
Write-Host ("== RESULT: {0} pass, {1} fail ==" -f $pass, $fail) -ForegroundColor $(if ($fail -eq 0) { 'Green' } else { 'Red' })
Write-Host ''
Write-Host 'Next steps:' -ForegroundColor Cyan
Write-Host "  - WPF (admin):  Start-Process `"$wpf`" -Verb RunAs"
Write-Host "  - Deploy new build: powershell -File `"$(Join-Path $root 'src\scripts\deploy.ps1')`"  then reboot"
Write-Host "  - Raw stats:    & `"$feat`" stats"
exit $(if ($fail -eq 0) { 0 } else { 1 })
