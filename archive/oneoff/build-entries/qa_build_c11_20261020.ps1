$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$msvc = "C:\Program1\VC\Tools\MSVC\14.44.35207"
$kit  = "C:\Program Files (x86)\Windows Kits\10"
$out  = Join-Path $root "src\core\build\qa-20261020"
$cl = Join-Path $msvc "bin\Hostx64\x64\cl.exe"
$inc = @("/I$root\src\core","/I$msvc\include","/I$kit\Include\10.0.26100.0\ucrt","/I$kit\Include\10.0.26100.0\um","/I$kit\Include\10.0.26100.0\shared","/I$root\src\core","/I$root\src\common","/I$root\src\core\usertest")
$libp = @("/LIBPATH:$msvc\lib\x64","/LIBPATH:$kit\Lib\10.0.26100.0\ucrt\x64","/LIBPATH:$kit\Lib\10.0.26100.0\um\x64")
$cf = @("/nologo","/O2","/W4","/utf-8","/MT")
foreach ($t in @("edge_v4","stress_v4")) {
    $of = "$env:TEMP\qa_cl_$t.txt"
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    & $cl $cf $inc "/c" "$root\src\core\$t.c" "/Fo:$out\$t.obj" 2>&1 | Out-File $of -Encoding utf8
    if ($LASTEXITCODE -ne 0) { Write-Output "$t COMPILE FAIL"; Get-Content $of | Select-Object -Last 6; $ErrorActionPreference=$old; continue }
    & $cl $cf $inc "$out\$t.obj" "$out\cache.obj" "/Fe:$out\$t.exe" "/Fm:$out\$t.map" "/link" $libp 2>&1 | Out-File $of -Encoding utf8
    $ErrorActionPreference = $old
    if ($LASTEXITCODE -ne 0) { Write-Output "$t LINK FAIL"; Get-Content $of | Select-Object -Last 6; continue }
    $warn = @(Get-Content $of | Where-Object { $_ -match "warning" }).Count
    Write-Output "$t`: BUILD OK (warnings=$warn)"
}
