# _build_driver.ps1 -- replicate RAMBlockCache.vcxproj Release|x64 with direct cl+link
# All flags taken verbatim from the vcxproj (see comments in RAMBlockCache.vcxproj):
#   defines: _AMD64_;_WIN64;RB_KERNEL_BUILD
#   include: SDK shared;SDK ucrt;WDK km;WDK shared;MSVC inc;core
#   link:    ntoskrnl.lib BufferOverflowK.lib wdmsec.lib, /DRIVER /NODEFAULTLIB
#           /SUBSYSTEM:NATIVE /ENTRY:GsDriverEntry /OPT:REF /OPT:ICF /DEBUG
# NOTE: keep this file ASCII-only (PS 5.1 reads BOM-less UTF-8 as ANSI).
$ErrorActionPreference = "Stop"
$drv  = $PSScriptRoot
$root = Split-Path (Split-Path $drv)                 # -> repo root
$msvc = "C:\Program1\VC\Tools\MSVC\14.44.35207"
$wdk  = Join-Path $root ".nuget\packages\microsoft.windows.wdk.x64\10.0.26100.6584\c"
$sdk  = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"   # installed WinSDK shared headers (junction-to-nuget blocked by mount-point trust)
$obj  = Join-Path $drv "x64\Release\obj"
$out  = Join-Path $drv "x64\Release"
New-Item -ItemType Directory -Force $obj, $out | Out-Null

$env:INCLUDE = "$sdk\shared;$sdk\ucrt;$wdk\Include\10.0.26100.0\km;$wdk\Include\10.0.26100.0\shared;$msvc\include;$root\src\core"
$env:LIB     = "$wdk\Lib\10.0.26100.0\km\x64;$msvc\lib\x64"
$env:PATH    = "$msvc\bin\Hostx64\x64;$env:SystemRoot\System32;$env:SystemRoot;$env:PATH"

$files = @("rbdriver","rbattach","rbio","rbctl","rb_stub","rbfeat","rbpools",
           "rbpref","rbhot","rbL2","rbwback","rbio_ports","rbthermal","rbetw","..\core\cache")
$fail = 0
foreach ($f in $files) {
    $name = Split-Path $f -Leaf
    $src = Join-Path $drv ($f + ".c")
    $fo  = Join-Path $obj ($name + ".obj")
    & cl /nologo /c /O2 /Oi /Gy /utf-8 /W3 /std:c11 /Zi /MT `
         /D_AMD64_ /D_WIN64 /DRB_KERNEL_BUILD `
         $src "/Fo$fo"
    if ($LASTEXITCODE -ne 0) { Write-Output "[FAIL-cl] $name"; $fail++ }
}
if ($fail -gt 0) { Write-Output "COMPILE_FAIL=$fail"; exit 1 }
Write-Output "COMPILE_OK=$($files.Count)"

$sys = Join-Path $out "RAMBlockCache.sys"
& link /NOLOGO "/OUT:$sys" /DRIVER /NODEFAULTLIB /MANIFEST:NO `
       /SUBSYSTEM:NATIVE /ENTRY:GsDriverEntry /MACHINE:X64 /OPT:REF /OPT:ICF /DEBUG `
       (Join-Path $obj "rbdriver.obj") (Join-Path $obj "rbattach.obj") (Join-Path $obj "rbio.obj") (Join-Path $obj "rbctl.obj") `
       (Join-Path $obj "rb_stub.obj") (Join-Path $obj "rbfeat.obj") (Join-Path $obj "rbpools.obj") (Join-Path $obj "rbpref.obj") `
       (Join-Path $obj "rbhot.obj") (Join-Path $obj "rbL2.obj") (Join-Path $obj "rbwback.obj") (Join-Path $obj "rbio_ports.obj") `
       (Join-Path $obj "rbthermal.obj") (Join-Path $obj "rbetw.obj") (Join-Path $obj "cache.obj") `
       ntoskrnl.lib BufferOverflowK.lib wdmsec.lib
if ($LASTEXITCODE -ne 0) { Write-Output "LINK_FAIL"; exit 1 }
Get-Item $sys | Select-Object FullName, Length, LastWriteTime
Write-Output "BUILD_OK"
