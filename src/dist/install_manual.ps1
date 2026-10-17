# Manual elevated install with per-step logging (ASCII-safe)
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}
$dist = Split-Path -Parent $PSCommandPath
$log = Join-Path $dist "install_manual.log"
if (Test-Path $log) { Remove-Item $log }
function L($m) { Add-Content -Path $log -Value $m -Encoding UTF8 }

L "1) files in dist:"
# [P2-11] 产物清单修正：旧清单写 RamBoostFS.Wpf.exe / RamBoostSetup.exe（均不存在），
# 实际发布物是 dist\RAMBlockCache.sys + dist\wpf\RAMBlockCache.Controller.exe(+dll)。
foreach ($n in "RAMBlockCache.sys","wpf\RAMBlockCache.Controller.exe","ramboostctl.exe") {
    L ("   {0} exists={1}" -f $n, (Test-Path (Join-Path $dist $n)))
}

L "2) copy sys to System32\drivers"
$dst = Join-Path $env:SystemRoot "System32\drivers\RAMBlockCache.sys"
try { Copy-Item (Join-Path $dist "RAMBlockCache.sys") $dst -Force -ErrorAction Stop; L "   sys copied OK" }
catch { L ("   COPY SYS FAIL: " + $_.Exception.Message) }

L "3) ProgramFiles copy"
$pf = Join-Path $env:ProgramFiles "RAMBoostFS"
New-Item -ItemType Directory -Force $pf | Out-Null
try {
    # [P2-11] 递归复制并保留相对目录：旧代码 `Get-ChildItem $dist -File`（不含子目录）
    # 导致 wpf\RAMBlockCache.Controller.exe 从不被部署。
    Get-ChildItem $dist -File -Recurse |
      Where-Object { $_.Name -notmatch 'env_report|install_manual|\.cmd$|\.log$' } |
      ForEach-Object {
          $rel = $_.FullName.Substring($dist.Length + 1)
          $target = Join-Path $pf $rel
          New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
          Copy-Item $_.FullName $target -Force
      }
    L "   pf copied OK (recursive)"
} catch { L ("   PF FAIL: " + $_.Exception.Message) }

L "4) create service"
$c = sc.exe create RAMBlockCache type= kernel start= demand error= normal binPath= $dst DisplayName= "RAMBlockCache RAM Disk Cache" 2>&1
L ("   " + ($c -join ' '))
sc.exe config RAMBlockCache loadordergroup= "Volume Filter" | Out-Null

L "5) UpperFilters"
$cls = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}"
$existing = @((Get-ItemProperty $cls -Name UpperFilters).UpperFilters) | Where-Object { $_ }
if ($existing -notcontains 'RAMBlockCache') {
    Set-ItemProperty $cls -Name UpperFilters -Value ($existing + 'RAMBlockCache')
}
L ("   now: " + (((Get-ItemProperty $cls).UpperFilters) -join ','))

L "6) start service"
$s = sc.exe start RAMBlockCache 2>&1
L ("   " + ($s -join ' '))

L "7) testsigning"
$t = bcdedit /enum | Select-String 'testsigning'
L ("   [" + ($t -join ';') + "]")
L "MANUAL_INSTALL_DONE"
