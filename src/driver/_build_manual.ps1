# _build_manual.ps1 - manual driver build (cl/link direct).
# Why this exists:
#   1) MSBuild.exe is blocked (LOLBin policy) in some environments
#   2) vcvars64.bat is blocked (reg.exe) -> INCLUDE/LIB/PATH set manually here
#   3) Git Bash mangles /O2-style args and UTF-8 paths -> run from PowerShell
# Replicates RAMBlockCache.vcxproj Release|x64:
#   WDM, v143, /O2 /Oi /Gy /MT /utf-8 /kernel
#   defines: _AMD64_;_WIN64;RB_KERNEL_BUILD
#   link: /DRIVER /NODEFAULTLIB /SUBSYSTEM:NATIVE /ENTRY:GsDriverEntry
#         ntoskrnl.lib BufferOverflowK.lib wdmsec.lib
# Header mix (NuGet WDK km + installed SDK shared/ucrt; NuGet sdk.cpp lacks
# ntdef.h so shared/ucrt come from the installed Windows Kits):
#   kit  = C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0
#   wdkn = <repo>\.nuget\packages\microsoft.windows.wdk.x64\<ver>\c\Include\10.0.26100.0
# Pure ASCII on purpose (GBK console lesson).
$ErrorActionPreference = "Stop"
# PSScriptRoot = <repo>\src\driver -> repo root is two levels up
$root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$msvc  = "C:\Program1\VC\Tools\MSVC\14.44.35207"
$kit   = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
$wdkn  = Join-Path $root ".nuget\packages\microsoft.windows.wdk.x64\10.0.26100.6584\c\Include\10.0.26100.0"
$wdklib= Join-Path $root ".nuget\packages\microsoft.windows.wdk.x64\10.0.26100.6584\c\Lib\10.0.26100.0\km\x64"

$cl   = Join-Path $msvc "bin\Hostx64\x64\cl.exe"
$link = Join-Path $msvc "bin\Hostx64\x64\link.exe"
if (-not (Test-Path -LiteralPath $cl))   { throw "cl.exe not found: $cl" }
if (-not (Test-Path -LiteralPath $link)) { throw "link.exe not found: $link" }

$out = Join-Path $root "src\driver\x64\Release"
$obj = Join-Path $out "obj"
New-Item -ItemType Directory -Force -Path $obj | Out-Null

$inc = @(
    "/I$kit\shared",
    "/I$kit\ucrt",
    "/I$wdkn\km",
    "/I$wdkn\shared",
    "/I$msvc\include",
    "/I$root\src\core",
    "/I$root\src\common"
)

$clCommon = @(
    "/nologo","/c","/TC","/std:c11","/O2","/Oi","/Gy","/Zi","/MT","/utf-8","/W3",
    "/wd4996","/wd4204","/wd4214","/wd4100","/wd4127","/wd4244","/wd4267","/wd4819","/wd4189","/wd4101",
    "/D_AMD64_","/D_WIN64","/DRB_KERNEL_BUILD","/kernel",
    "/Fo$obj\"
)

$srcs = @("rbdriver.c","rbattach.c","rbio.c","rbctl.c","rb_stub.c","rbfeat.c",
          "rbpools.c","rbpref.c","rbhot.c","rbL2.c","rbwback.c","rbio_ports.c",
          "rbthermal.c","rbetw.c","..\core\cache.c")

Push-Location (Join-Path $root "src\driver")
try {
    Write-Host "[1/2] compiling $($srcs.Count) sources ..."
    $fail = 0
    foreach ($s in $srcs) {
        & $cl @clCommon @inc $s
        if ($LASTEXITCODE -ne 0) { Write-Host "  [X] $s FAILED (exit $LASTEXITCODE)"; $fail++ }
        else { Write-Host "  [ok] $s" }
    }
    if ($fail -gt 0) { throw "$fail source(s) failed to compile" }

    Write-Host "[2/2] linking RAMBlockCache.sys ..."
    & $link @(
        "/nologo","/DRIVER","/NODEFAULTLIB","/MANIFEST:NO","/SUBSYSTEM:NATIVE",
        "/ENTRY:GsDriverEntry","/MACHINE:X64","/DEBUG","/RELEASE","/OPT:REF","/OPT:ICF",
        "ntoskrnl.lib","hal.lib","BufferOverflowK.lib","wdmsec.lib",
        "/LIBPATH:$wdklib",
        "/OUT:$out\RAMBlockCache.sys",
        "$obj\*.obj"
    )
    if ($LASTEXITCODE -ne 0) { throw "link failed (exit $LASTEXITCODE)" }
}
finally { Pop-Location }

$sys = Join-Path $out "RAMBlockCache.sys"
Write-Host "=== BUILD OK ==="
Get-Item -LiteralPath $sys | Select-Object FullName, Length, LastWriteTime | Format-List
$h = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash
Write-Host "SHA256 = $h"
Write-Host "NOTE: this .sys is UNSIGNED - sign before deploy (signtool)."
Write-Host "NOTE: after any source change the deploy anchors must be re-synced:"
Write-Host "      src\scripts\deploy.ps1 / verify_deploy_signature.ps1 / README.txt"
