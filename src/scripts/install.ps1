# install.ps1 —— 注册 RAMBlockCache 服务 + 给 Volume 类加 UpperFilters
# 双击 install.cmd 运行（自动 UAC 提权）。
# 注意：这是“唯一可能需要一次重启”的步骤（让 PnP 在既有卷栈上挂过滤设备）。
. (Join-Path $PSScriptRoot 'rb_common.ps1')
Assert-Admin

# 1) 编译（Release|x64）
$env:NUGET_PACKAGES = $Script:NugetPkg
& $Script:MSBuild (Join-Path $Script:DriverDir 'RAMBlockCache.vcxproj') `
    /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
if ($LASTEXITCODE -ne 0) { throw '编译失败' }

# 2) 签名 + 落地
# [FIX N1 2026-10-19] 验签必须在 Install-DriverBinary（System32 覆盖）之前：
# 原顺序先覆盖 System32 再验签——验签失败时机器已带着 BOOT_START + UpperFilters
# + 未验证二进制等待重启 = 0x7B 同型。现对刚签名的构建产物（$Script:SysPath）
# 双验签，失败即终止且机器零变更；复制后的副本不再重复验签（复制的是已验文件）。
Sign-Driver
$sig = Get-AuthenticodeSignature -LiteralPath $Script:SysPath -ErrorAction Stop
if ($sig.Status -ne 'Valid') { throw "驱动签名校验失败（$($sig.Status)）——拒绝安装。请先签名（见 README 签名节）。" }
$st = $Script:SignTool
& $st verify /pa $Script:SysPath 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "signtool verify /pa 失败——拒绝安装（exit=$LASTEXITCODE）" }
Write-Host "[ok] 驱动签名校验通过（$($sig.SignerCertificate.Subject)）"
Install-DriverBinary

# 3) 创建 PnP 过滤服务（SERVICE_DEMAND_START，不随开机自动加载）
# [FIX 致命-3 2026-10-18] 三重整改：
#   ① sc.exe delete 对运行中服务只打删除标记，紧随的 create 必返 1072
#      （ERROR_SERVICE_MARKED_FOR_DELETE）而旧代码照报 [ok] → 重启后卷类引用
#      已删除服务。现：先 Stop-Service 并确认停止（15s 上限），每步 sc.exe
#      检查 $LASTEXITCODE，失败即 throw（fail-closed）。
#   ② 全文零签名校验：本路径曾经是 0x7B 事故的 fail-open 变体。现补
#      Get-AuthenticodeSignature + signtool verify /pa 双验签（[FIX N1] 已前移到
#      System32 覆盖之前），未签名/验签失败即终止，绝不写 UpperFilters。
#   ③ 补 armed 基线（FilterArmed=1 + BootFailCount=0），与 deploy.ps1 第 3 步
#      一致，让 boot_guard.ps1 的回滚护栏在本路径同样生效。
# [FIX N-b] 统一入口口径（原与 ramboostsetup.ps1:15-21 互斥）：普通用户=
#   dist\RAMBlockCache-Setup.exe；开发者/脚本=deploy.ps1；本脚本与
#   ramboostsetup.ps1 均为 legacy 备选（详见 README"安装"节）。
# [FIX 回归B 2026-10-18 复审] 验签必须在停服/删服之前（与 ramboostsetup.c 严重-1
# 同型纪律：验签失败时机器保持原状，不做任何破坏性动作——原修复把验签放在了
# Stop-Service + sc delete 之后，失败路径仍会拆掉旧安装）。
$sysPath = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'

$svc = Get-Service $Script:ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -ne 'Stopped') {
    Write-Host "[..] 停止运行中的 $Script:ServiceName（最多 15s）..."
    Stop-Service $Script:ServiceName -Force -ErrorAction SilentlyContinue
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Service $Script:ServiceName).Status -ne 'Stopped' -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 300
    }
    if ((Get-Service $Script:ServiceName).Status -ne 'Stopped') {
        throw '旧服务 15s 内未停止（驱动可能停不掉——C3 设计保数据）。请重启后再安装，或改用 deploy.ps1。'
    }
}
sc.exe delete $Script:ServiceName | Out-Null
if ($LASTEXITCODE -notin @(0, 1060)) { throw "sc.exe delete 失败（exit=$LASTEXITCODE）" }

sc.exe create $Script:ServiceName `
    type= kernel start= demand error= normal `
    binPath= $sysPath `
    DisplayName= 'RAMBlockCache RAM Disk Cache' | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sc.exe create 失败（exit=$LASTEXITCODE，1072=服务标记删除未完成，需重启）" }
sc.exe config $Script:ServiceName loadordergroup= 'Volume Filter' | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sc.exe config 失败（exit=$LASTEXITCODE）" }
Write-Host '[ok] 服务已注册（demand start）'

# 4) 把 RAMBlockCache 追加到 Volume 类 UpperFilters（多字符串追加，不覆盖既有）
$cls = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$($Script:VolumeClassGuid)"
if (-not (Test-Path $cls)) { New-Item -Path $cls -Force | Out-Null }
$existing = (Get-ItemProperty -Path $cls -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
if (-not $existing) { $existing = @() }
if ($existing -notcontains $Script:ServiceName) {
    $new = @($existing) + $Script:ServiceName
    New-ItemProperty -Path $cls -Name UpperFilters -PropertyType MultiString -Value $new -Force | Out-Null
    Write-Host '[ok] 已加入 Volume UpperFilters'
} else {
    Write-Host '[..] UpperFilters 已包含 RAMBlockCache'
}

# 4b) [FIX 致命-3 2026-10-18] armed 基线：boot_guard.ps1 回滚护栏生效的前提
# （原路径不写 FilterArmed → boot_guard.ps1 首行即 exit 0、护栏空转）。
# 与 deploy.ps1 第 3 步一致；安装成功后由 logon_disarm 机制解除（同一生命周期）。
$parm = "HKLM:\SYSTEM\CurrentControlSet\Services\$($Script:ServiceName)\Parameters"
New-Item -Path $parm -Force | Out-Null
New-ItemProperty -Path $parm -Name FilterArmed -PropertyType DWord -Value 1 -Force | Out-Null
New-ItemProperty -Path $parm -Name BootFailCount -PropertyType DWord -Value 0 -Force | Out-Null
Write-Host '[ok] armed 基线已写（boot_guard 回滚护栏生效）'

# 5) 触发重新枚举存储设备（免重启尝试；若无效则需一次重启）
# [P2-13] 不再对全部 Volume 设备 Disable/Enable：那是对整个存储栈的重枚举
# 风暴，可能中断在途 I/O、使系统卷短暂掉线。改为只刷新主机存储缓存；
# 挂栈仍由下一次重启或卷到达事件由 PnP 完成。
Write-Host '[..] 刷新存储缓存（不再强制重枚举所有卷）...'
try {
    Update-HostStorageCache -ErrorAction Stop
    Write-Host '[ok] 存储缓存已刷新'
} catch {
    Write-Host '[..] 刷新存储缓存不可用（可忽略；重启后由 PnP 挂栈）'
}

Write-Host ''
Write-Host '完成。若过滤设备未在卷栈上出现，请重启一次（这是唯一可能需要的一次重启）。'
Write-Host '重启后：驱动 AddDevice 会挂到卷栈，默认缓存关闭；用 ctl/WPF 对 F/D 执行 ATTACH_VOLUME 即开始加速。'
Read-Host '按回车关闭'
