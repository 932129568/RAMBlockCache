# RAMBlockCache 部署公共配置（被各脚本 dot-source）
#
# [P0-3] SignTool/MSBuild/NugetPkg 参数化（原三处硬编码路径）：
#   解析优先级：显式 param() 传参 > 环境变量 > 下方默认值（与重构前逐字符相同）。
#   用法示例：
#     . (Join-Path $PSScriptRoot 'rb_common.ps1') -SignTool 'D:\Kits\...\signtool.exe'
#     $env:RB_MSBUILD = 'D:\...\MSBuild.exe'; . (Join-Path $PSScriptRoot 'rb_common.ps1')
#   注意：默认值中的失效路径（MSBuild=C:\Program1\...；NugetPkg=历史 chat 目录）
#   现状只在变更日志/上报中标注，本次刻意不改变默认解析结果（零行为变更）；
#   与规格书 P0-3 原文的"存在性回退探测"的差异说明见变更日志（探测会改变解析顺序）。
#   现有三个消费方（install.ps1 / game_mode.ps1 / run_diskspd.ps1）均为无参 dot-source，
#   解析结果与重构前完全一致。
param(
    [string]$SignTool,
    [string]$MSBuild,
    [string]$NugetPkg
)
$ErrorActionPreference = 'Stop'

# 工程根 = scripts\ 的上一级
$Script:ProjectRoot = Split-Path -Parent $PSScriptRoot

$Script:DriverDir   = Join-Path $ProjectRoot 'driver'
$Script:SysPath      = Join-Path $DriverDir 'x64\Release\RAMBlockCache.sys'
$Script:InfPath      = Join-Path $DriverDir 'RAMBlockCache.inf'
$Script:ServiceName  = 'RAMBlockCache'
$Script:VolumeClassGuid = '{71a27cdd-812a-11d0-bec7-08002be2092f}'

$Script:SignTool  = if ($SignTool) { $SignTool } elseif ($env:RB_SIGNTOOL) { $env:RB_SIGNTOOL } else { 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe' }
$Script:CertThumb = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431'
$Script:MSBuild   = if ($MSBuild)  { $MSBuild }  elseif ($env:RB_MSBUILD)  { $env:RB_MSBUILD }  else { 'C:\Program1\MSBuild\Current\Bin\MSBuild.exe' }
$Script:NugetPkg  = if ($NugetPkg) { $NugetPkg } elseif ($env:RB_NUGETPKG) { $env:RB_NUGETPKG } else { 'C:\Users\陈\Doubao\chats\2026-10-10\new-chat-1\v6work\.nuget\packages' }

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '需要管理员权限。请通过 .cmd 启动以触发 UAC 自提权。'
    }
}

function Install-DriverBinary {
    $dest = Join-Path $env:SystemRoot 'System32\drivers\RAMBlockCache.sys'
    Copy-Item $Script:SysPath $dest -Force
    Write-Host "[ok] 已部署 $dest"
}

function Sign-Driver {
    if (-not (Test-Path $Script:SysPath)) { throw "未找到 $($Script:SysPath)，请先 build。" }
    Write-Host "[..] 签名 $($Script:SysPath)"
    & $Script:SignTool sign /v /sha1 $Script:CertThumb /fd SHA256 `
        /t http://timestamp.digicert.com $Script:SysPath
    if ($LASTEXITCODE -ne 0) { throw 'signtool 签名失败' }
    Write-Host '[ok] 签名完成'
}
