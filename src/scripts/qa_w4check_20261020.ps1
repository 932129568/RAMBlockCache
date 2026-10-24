$msvc = "C:\Program1\VC\Tools\MSVC\14.44.35207"
$kit  = "C:\Program Files (x86)\Windows Kits\10"
$root = "C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"
$cl = Join-Path $msvc "bin\Hostx64\x64\cl.exe"
$inc = @("/I$root\src\core","/I$msvc\include","/I$kit\Include\10.0.26100.0\ucrt","/I$kit\Include\10.0.26100.0\um","/I$kit\Include\10.0.26100.0\shared","/I$root\src\common","/I$root\src\core\usertest")
$logf = "$env:TEMP\qa_cache_w4.log"
$old = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $cl /nologo /W4 /utf-8 /MT $inc /c "$root\src\core\cache.c" /Fo:"$env:TEMP\qa_cache_w4.obj" 2>&1 | Out-File -FilePath $logf -Encoding utf8
$rc = $LASTEXITCODE
$ErrorActionPreference = $old
$lines = @(Get-Content $logf)
$w = @($lines | Where-Object { $_ -match "warning" }).Count
Write-Output "cache.c /W4 compile rc=$rc warnings=$w"
$lines | Select-Object -First 10
