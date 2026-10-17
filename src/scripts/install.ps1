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
Sign-Driver
Install-DriverBinary

# 3) 创建 PnP 过滤服务（SERVICE_DEMAND_START，不随开机自动加载）
sc.exe delete $Script:ServiceName | Out-Null
sc.exe create $Script:ServiceName `
    type= kernel start= demand error= normal `
    binPath= (Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys') `
    DisplayName= 'RAMBlockCache RAM Disk Cache' | Out-Null
sc.exe config $Script:ServiceName loadordergroup= 'Volume Filter' | Out-Null
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
