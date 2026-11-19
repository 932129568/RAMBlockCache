# pfro_pending.ps1 -- PFRO 排队替换纯函数库 [FIX S-04]
# 层：Scripts。职责：把 verify_deploy_signature.ps1 的 -AllowPendingReplace 匹配逻辑
# 抽为可 dot-source 的纯函数（无系统访问、无顶层副作用），供 verify 运行时侧与
# QA 测试侧复用；deploy.ps1 与 verify 之间是子进程调用而非 dot-source，deploy 侧
# "整值备份/恢复 + 断言"不需要配对匹配，故不 dot-source 本库（库对其开放，零副作用）。
#
# PFRO = HKLM\SYSTEM\CurrentControlSet\Control\Session Manager 的
# PendingFileRenameOperations（REG_MULTI_SZ），条目扁平排布为
# [源1, 目标1, 源2, 目标2, ...]，步长 2 切对；奇数尾元素丢弃
# （REG_MULTI_SZ 理应成对，畸形数据不参与匹配，见设计假设 A4）。
#
# 匹配语义（fail-closed）：源（归一化）== 期望源 且 目标（归一化）== 期望目标
# 才 $true；空数组 / 陈旧队列（源是旧 dist 路径的旧二进制）-> $false。
#
# 本文件必须保持 UTF-8 with BOM（check_ps_syntax.ps1 断言）。

# 去 \??\ / \\?\ 前缀 + Trim + ToLowerInvariant（大小写不敏感归一化）。
# 注意：PowerShell 单引号串无反斜杠转义，'\??\' 即 4 字符 \??\，'\\?\' 即 4 字符 \\?\。
function Remove-PfroPathPrefix([string]$Path) {
    if ([string]::IsNullOrEmpty($Path)) { return '' }
    $p = $Path.Trim()
    if ($p.StartsWith('\??\')) { $p = $p.Substring(4) }
    elseif ($p.StartsWith('\\?\')) { $p = $p.Substring(4) }
    return $p.ToLowerInvariant()
}

# 按 REG_MULTI_SZ 扁平结构步长 2 切 (Source,Target) 对；奇数尾元素丢弃。
# return ,$pairs 保空数组语义（空输入返回空数组而非 $null）。
function Split-PfroPairs([string[]]$Entries) {
    $pairs = @()
    if ($null -ne $Entries -and $Entries.Count -ge 2) {
        $n = 2 * [int][math]::Floor($Entries.Count / 2)
        for ($i = 0; $i -lt $n; $i += 2) {
            $pairs += [pscustomobject]@{ Source = $Entries[$i]; Target = $Entries[$i + 1] }
        }
    }
    return ,$pairs
}

# 成对匹配：源（归一化）== $ExpectedSource 且 目标（归一化）== $ExpectedTarget 才 $true。
# 空数组 / 陈旧队列 -> $false。纯函数：不读注册表、不碰磁盘。
function Test-PfroPendingReplace([string[]]$PfroEntries, [string]$ExpectedSource, [string]$ExpectedTarget) {
    if ($null -eq $PfroEntries -or $PfroEntries.Count -lt 2) { return $false }
    $src = Remove-PfroPathPrefix $ExpectedSource
    $dst = Remove-PfroPathPrefix $ExpectedTarget
    if ([string]::IsNullOrEmpty($src) -or [string]::IsNullOrEmpty($dst)) { return $false }
    foreach ($pair in (Split-PfroPairs $PfroEntries)) {
        if ((Remove-PfroPathPrefix $pair.Source) -eq $src -and (Remove-PfroPathPrefix $pair.Target) -eq $dst) {
            return $true
        }
    }
    return $false
}
