#requires -Version 5.1
<#
.SYNOPSIS
    PowerShell 语法与编码门禁（架构文档风险登记册 R4）。
.DESCRIPTION
    三类检查，任一失败退出码 1（可作提交前门禁 / CI 步骤）：
      1) BOM：所有活跃 .ps1 必须为 UTF-8 with BOM。Windows PowerShell 5.1
         对无 BOM 的 .ps1 按 ANSI 代码页解析，含中文的脚本会语法损坏——
         2026-10-15 本轮实测 13 例（含部署门禁 verify_deploy_signature.ps1、
         setup/ramboostsetup.ps1），全部已补 BOM。
      2) 语法：[Parser]::ParseFile 零解析错误。
      3) [FIX G-01] 批处理编码：所有活跃 .cmd/.bat 必须纯 ASCII 且无 UTF-8
         BOM——把 S-12 中文 REM 英文化成果制度钉死，不回流。排除清单与
         输出格式同 .ps1 检查逐字同构。
      4) [FIX #9] 运行时绑定惯用法缺陷（AST 查不出、首发即崩类，两种机制）：
         a. 重复开关参数——同一开关相邻连写两次（如 Split-Path -Parent 连写
            两次）在 PS 5.1 报 ParameterAlreadyBound；2026-10-21 实测 4 例
            （qa_teeth/qa_sweep_wsconc/qa_smoke_wsconc/ramboostsetup.ps1）
            全部首发即崩。按 AST 挖空注释 token 后对开关白名单做相邻重复匹配。
         b. Write-Host 误用格式运算符——Write-Host "..." -f $x 的 -f 被绑定成
            -ForegroundColor 缩写前缀（2026-10-21 qa_smoke_wsconc 实测：组合名
            default 当颜色值转换即崩）。判别式：Write-* 后紧跟字符串（非左括号）
            再跟 -f；正确写法 Write-Host ("..." -f $x) 不误报。
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

$fail = 0; $bomMiss = 0; $parseErr = 0; $cmdChr = 0; $cmdBom = 0; $dupSw = 0
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

# ---- 第 3 类：.cmd/.bat 纯 ASCII + 无 BOM（[FIX G-01]）----
# 与 .ps1 段逐字同构：同一 $exclude 数组、同一 Where 管道、同一输出格式。
$exclude = @('\src.backup-', '\RAMBlockCache-Build16\', '\obj\', '\bin\', '\build\')
$cmdFiles = Get-ChildItem -Path $Root -Recurse -Include *.cmd,*.bat -File |
    Where-Object {
        $p = $_.FullName
        -not ($exclude | Where-Object { $p -like "*$_*" })
    }

foreach ($f in $cmdFiles) {
    $rel = $f.FullName.Substring($Root.Length).TrimStart('\')
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    if ($hasBom) {
        Write-Host ("[FAIL][BOM ] {0}" -f $rel)
        $cmdBom++; $fail++
        continue
    }
    $nonAscii = 0
    foreach ($b in $bytes) { if ($b -gt 0x7F) { $nonAscii++ } }
    if ($nonAscii -gt 0) {
        Write-Host ("[FAIL][CHR ] {0} ({1} non-ASCII bytes)" -f $rel, $nonAscii)
        $cmdChr++; $fail++
    }
    else {
        Write-Host ("[ OK ] {0}" -f $rel)
    }
}
# ---- 第 4 类：运行时绑定惯用法缺陷（[FIX #9]；AST 查不出、首发即崩类）----
# 机制一（重复开关）：PS 5.1 对"同一开关参数相邻连写两次"报 ParameterAlreadyBound
# （实测 Split-Path -Parent 连写两次、经 -File 通道同样崩），而 [Parser]::ParseFile
# 只看语法不看绑定，旧门禁因此漏过 4 例首发即崩脚本。这里按 AST 先把注释 token
# 挖空后（保留换行保行号），再对开关白名单做相邻重复匹配——注释里的教学性
# 提及不误报，字符串/真实代码中的重复一律 FAIL。
# 机制二（Write-Host 误用格式运算符）：Write-Host "..." -f $x 的 -f 会被绑定成
# -ForegroundColor 的缩写前缀（2026-10-21 qa_smoke_wsconc 实测：组合名 default
# 当颜色值转换即崩）。正确写法是先算后传 Write-Host ("..." -f $x) -ForegroundColor。
# 判别式：Write-* 之后紧跟引号字符串（而非左括号）再跟 -f——父元形式不误报。
$swNames = 'Parent|Force|Recurse|Wait|PassThru|AsJob|Confirm|Verbose|Debug|WhatIf|NoProfile|NoNewWindow|NoClobber|NoRecurse|Interactive'
$dupRe   = [regex]::new(("(-(?:{0}))[ \t]+\1\b" -f $swNames), 'IgnoreCase')
$fmtRe   = [regex]::new('Write-(?:Host|Output|Warning|Error)\s+"[^"]*"\s+-f\b', 'IgnoreCase')
foreach ($f in $files) {
    $rel = $f.FullName.Substring($Root.Length).TrimStart('\')
    $tokens2 = $null; $errors2 = $null
    [System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens2, [ref]$errors2) | Out-Null
    if ($errors2 -and $errors2.Count -gt 0) { continue }   # 语法错已在第 2 类报过
    $text = [System.IO.File]::ReadAllText($f.FullName)
    $sb = New-Object System.Text.StringBuilder $text
    foreach ($tk in $tokens2) {
        if ($tk.Kind -eq 'Comment') {
            for ($i = $tk.Extent.StartOffset; $i -lt $tk.Extent.EndOffset; $i++) {
                $ch = $text[$i]
                if ($ch -ne "`r" -and $ch -ne "`n") { $sb[$i] = ' ' }
            }
        }
    }
    $lineNo = 0
    foreach ($line in ($sb.ToString() -split "`n")) {
        $lineNo++
        if ($dupRe.IsMatch($line)) {
            Write-Host ("[FAIL][DUPSW] {0}:{1}  {2}" -f $rel, $lineNo, $dupRe.Match($line).Value)
            $dupSw++; $fail++
        }
        if ($fmtRe.IsMatch($line)) {
            Write-Host ("[FAIL][FMT  ] {0}:{1}  {2}" -f $rel, $lineNo, $fmtRe.Match($line).Value)
            $dupSw++; $fail++
        }
    }
}

Write-Host ("== check_ps_syntax == files={0} fail={1} (bom_miss={2} syntax_err={3} dup_sw={4}) cmd_files={5} cmd_chr={6} cmd_bom={7}" -f $files.Count, $fail, $bomMiss, $parseErr, $dupSw, $cmdFiles.Count, $cmdChr, $cmdBom)
if ($fail -gt 0) { exit 1 }
exit 0
