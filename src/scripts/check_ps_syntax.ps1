#requires -Version 5.1
<#
.SYNOPSIS
    PowerShell 语法与编码门禁（架构文档风险登记册 R4）。
.DESCRIPTION
    两类检查，任一失败退出码 1（可作提交前门禁 / CI 步骤）：
      1) BOM：所有活跃 .ps1 必须为 UTF-8 with BOM。Windows PowerShell 5.1
         对无 BOM 的 .ps1 按 ANSI 代码页解析，含中文的脚本会语法损坏——
         2026-10-15 本轮实测 13 例（含部署门禁 verify_deploy_signature.ps1、
         setup/ramboostsetup.ps1），全部已补 BOM。
      2) 语法：[Parser]::ParseFile 零解析错误。
    有意排除（详见 architecture-and-work-breakdown-2026-10-15.md）：
      - src.backup-*                    整树备份
      - src\dist\RAMBlockCache-Build16  历史只读回滚存档（其 README 声明不得改动）
      - obj\ bin\ build\                编译中间产物目录
.PARAMETER Root
    扫描根目录，默认为本脚本所在 scripts\ 的上一级（src\）。
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File src\scripts\check_ps_syntax.ps1
.EXAMPLE
    powershell -NoProfile -File src\scripts\check_ps_syntax.ps1 -Root C:\pkg
#>
[CmdletBinding()]
param(
    [string]$Root = ''
)
$ErrorActionPreference = 'Stop'

# 自定位扫描根：优先 $PSScriptRoot → $MyInvocation → 约定路径 <pkg>\src\scripts。
# 不单独依赖 $PSScriptRoot：某些调用上下文（包装执行）中它可能为空。
if ([string]::IsNullOrWhiteSpace($Root)) {
    $here = $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($here)) {
        try { $here = Split-Path -Parent $MyInvocation.MyCommand.Path } catch { }
    }
    if ([string]::IsNullOrWhiteSpace($here)) {
        $here = Join-Path (Get-Location).Path 'scripts'
    }
    $Root = Split-Path -Parent $here
}
if (-not (Test-Path -LiteralPath $Root)) {
    Write-Host ("[FAIL] 扫描根不存在: {0}（请用 -Root 显式指定，如 -Root <包>\src）" -f $Root)
    exit 1
}
Write-Host ("== check_ps_syntax ==  root={0}" -f $Root)

$exclude = @('\src.backup-', '\RAMBlockCache-Build16\', '\obj\', '\bin\', '\build\')
$files = Get-ChildItem -Path $Root -Recurse -Filter *.ps1 -File |
    Where-Object {
        $p = $_.FullName
        -not ($exclude | Where-Object { $p -like "*$_*" })
    }

$fail = 0; $bomMiss = 0; $parseErr = 0
foreach ($f in $files) {
    $rel = $f.FullName.Substring($Root.Length).TrimStart('\')
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    if (-not $hasBom) {
        Write-Host ("[FAIL][BOM ] {0}" -f $rel)
        $bomMiss++; $fail++
        continue
    }
    $tokens = $null; $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens, [ref]$errors) | Out-Null
    if ($errors -and $errors.Count -gt 0) {
        Write-Host ("[FAIL][SYN ] {0}" -f $rel)
        foreach ($e in $errors) { Write-Host ("           {0}" -f $e.Message) }
        $parseErr++; $fail++
    }
    else {
        Write-Host ("[ OK ] {0}" -f $rel)
    }
}
Write-Host ("== check_ps_syntax == files={0} fail={1} (bom_miss={2} syntax_err={3})" -f $files.Count, $fail, $bomMiss, $parseErr)
if ($fail -gt 0) { exit 1 }
exit 0
