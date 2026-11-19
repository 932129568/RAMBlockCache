# copy_dumps.ps1 -- 提权：把崩溃转储中转到用户可读目录（U-8 归因准备，2026-11-06）
# Minidump 与 RAMBoost\dumps 的 ACL 仅管理员可读；cdb 分析需要反复跑，
# 故提权一次性拷贝到 C:\Users\Public\rbc-dump-analysis（后续主会话直接分析）。
$ErrorActionPreference = 'Continue'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '[X] must run elevated'
    exit 1
}
$dst = 'C:\Users\Public\rbc-dump-analysis'
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$log = Join-Path $dst 'copy-log.txt'
Remove-Item $log -ErrorAction SilentlyContinue
$n = 0
foreach ($s in @('C:\Windows\Minidump', 'C:\ProgramData\RAMBoost\dumps')) {
    if (-not (Test-Path -LiteralPath $s)) { continue }
    Get-ChildItem -LiteralPath $s -Filter *.dmp -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $dst $_.Name) -Force
        Add-Content -LiteralPath $log -Value ("{0}  {1} bytes  from {2}" -f $_.Name, $_.Length, $s)
        $n++
    }
}
Get-ChildItem 'C:\Windows\LiveKernelReports' -Recurse -Filter *.dmp -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $dst ('LKR_' + $_.Name)) -Force
    Add-Content -LiteralPath $log -Value ("LKR_{0}  {1} bytes" -f $_.Name, $_.Length)
    $n++
}
Write-Host ("copied {0} dumps to {1}" -f $n, $dst)
exit 0
