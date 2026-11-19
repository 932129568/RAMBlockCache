# tools/_cxxcheck_native_host.ps1  [295 2026-11-19]
# Compile-only check for src/ui/native-host -- proves these sources actually
# compile against real MSVC + WinSDK + WebView2 SDK on this machine.
# The scheme document shipped them without ever compiling (295 section 2).
# Pure ASCII body; run with pwsh.

$ErrorActionPreference = 'Continue'
$repo    = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$clRoot  = 'C:\Program1\VC\Tools\MSVC\14.44.35207'
$sdk     = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer  = '10.0.26100.0'
$wv2     = Join-Path $env:USERPROFILE '.nuget\packages\microsoft.web.webview2\1.0.2792.45\build\native\include'

$cl = Join-Path $clRoot 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path -LiteralPath $cl))      { Write-Host "FAIL: cl.exe missing at $cl"; exit 2 }
if (-not (Test-Path -LiteralPath (Join-Path $wv2 'WebView2.h'))) { Write-Host "FAIL: WebView2.h missing at $wv2"; exit 2 }

$env:INCLUDE = (@(
    (Join-Path $clRoot 'include'),
    (Join-Path $sdk "Include\$sdkVer\um"),
    (Join-Path $sdk "Include\$sdkVer\shared"),
    (Join-Path $sdk "Include\$sdkVer\ucrt"),
    (Join-Path $sdk "Include\$sdkVer\winrt"),
    $wv2
) -join ';')
$env:LIB = (@(
    (Join-Path $clRoot 'lib\x64'),
    (Join-Path $sdk "Lib\$sdkVer\um\x64"),
    (Join-Path $sdk "Lib\$sdkVer\ucrt\x64")
) -join ';')
$env:PATH = (Join-Path $clRoot 'bin\Hostx64\x64') + ';' + $env:PATH

$out = Join-Path $env:TEMP 'rbc-nativehost-check'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$srcDir = Join-Path $repo 'src\ui\native-host\src'
$rc = 0
foreach ($f in @('glass_window.cpp', 'desktop_capture.cpp')) {
    Write-Host "--- $f ---"
    $args = @(
        '/nologo', '/c', '/std:c++20', '/EHsc', '/W3', '/permissive-', '/utf-8',
        '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/D_WIN32_WINNT=0x0A00',
        "/Fo$out\$([IO.Path]::GetFileNameWithoutExtension($f)).obj",
        "/Fd$out\$([IO.Path]::GetFileNameWithoutExtension($f)).pdb",
        "/I$srcDir",
        (Join-Path $srcDir $f)
    )
    & $cl @args 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { $rc = 1 }
}

Write-Host ''
if ($rc -eq 0) { Write-Host 'COMPILE-CHECK OK' } else { Write-Host 'COMPILE-CHECK FAILED' }
exit $rc