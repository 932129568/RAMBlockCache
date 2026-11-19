# 钉「消费方」而非「声明方」：直接从交付脚本里抽出 Get-FlagsInt 函数体来测，
# 不复制一份手抄副本 —— 副本一旦漂移，这个验证就变成空跑。
$p = "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\deliverables\RB-postreboot-verify.ps1"
$src = [IO.File]::ReadAllText($p)
$b = [IO.File]::ReadAllBytes($p)
"file: bytes=$($b.Length) BOM=$([BitConverter]::ToString($b[0..2]))"
$crlf = ([regex]::Matches($src, "`r`n")).Count
$lf = ([regex]::Matches($src, "`n")).Count
"file: crlf=$crlf totalLF=$lf bareLF=$($lf - $crlf)"

$err = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($p, [ref]$null, [ref]$err)
if ($err.Count -eq 0) { "file: PARSE OK" } else { $err | ForEach-Object { $_.Message } }

# 抽取真实函数定义
$m = [regex]::Match($src, '(?ms)^function Get-FlagsInt.*?^\}')
if (-not $m.Success) { "EXTRACT FAIL: Get-FlagsInt 未找到"; exit 2 }
$body = $m.Value
"extract: $($body.Split("`n").Count) 行"

. ([ScriptBlock]::Create($body))

$cases = @(
    @{ raw = '    Flags    REG_DWORD    0xffffffff';  exp = [uint32]::MaxValue; label = '0xffffffff (全开)' },
    @{ raw = '    Flags    REG_DWORD    0x0';         exp = [uint32]0;          label = '0x0 (全关)' },
    @{ raw = '    Flags    REG_DWORD    0x18';        exp = [uint32]24;         label = '0x18 (L2+WB)' },
    @{ raw = '    Flags    REG_DWORD    0x1FFFFFFFF'; exp = $null;             label = '0x1FFFFFFFF (超宽 -> null)' },
    @{ raw = '    Flags    REG_DWORD    0x100000000'; exp = $null;             label = '0x100000000 (9 位 -> null)' },
    @{ raw = '    Flags    REG_DWORD    0xFFFFFFFFF'; exp = $null;             label = '0xFFFFFFFFF (9 位 -> null)' },
    @{ raw = '    Flags    REG_DWORD    0XFF';        exp = [uint32]255;        label = '0XFF (大写前缀)' },
    @{ raw = '    Flags    REG_DWORD    -1';          exp = [uint32]::MaxValue; label = '-1 (负数按位截断)' },
    @{ raw = '    Flags    REG_DWORD    4294967295';  exp = [uint32]::MaxValue; label = '4294967295 (十进制全开)' },
    @{ raw = '    Flags    REG_DWORD    4294967296';  exp = [uint32]0;          label = '4294967296 (超宽十进制, 按位截断)' },
    @{ raw = $null;                                   exp = $null;             label = '<absent>' }
)

$pass = 0
$fail = 0
foreach ($c in $cases) {
    $got = Get-FlagsInt $c.raw
    $ok = if ($null -eq $c.exp) { $null -eq $got } else { ($null -ne $got) -and ($got -eq $c.exp) }
    if ($ok) { $pass++ } else { $fail++ }
    '{0,-44} => {1,-12} {2}' -f $c.label, $(if ($null -eq $got) { '(null)' } else { $got }), $(if ($ok) { 'PASS' } else { 'FAIL' })
}

# 缺陷复现对照：旧实现必须在同一输入上给出错值（证明这条判据有牙，不是空跑）
$oldImpl = [ScriptBlock]::Create('
param([string]$Raw)
if (-not $Raw) { return $null }
if ($Raw -match ''0x([0-9a-fA-F]+)'') { return [Convert]::ToInt32($Matches[1], 16) }
if ($Raw -match ''(\d+)'') { return [int]$Matches[1] }
return $null
')
$oldGot = & $oldImpl '    Flags    REG_DWORD    0xffffffff'
$oldSame = ($null -ne $oldGot) -and ($oldGot -eq [uint32]::MaxValue)
"对照 旧实现 0xffffffff => $oldGot ; == [uint32]::MaxValue ? $oldSame (期望 False)"

"SUMMARY pass=$pass fail=$fail"
if ($fail -ne 0 -or $oldSame) { exit 1 }