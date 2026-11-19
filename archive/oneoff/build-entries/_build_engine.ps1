# _build_engine.ps1 -- [88 B-2 2026-11-07] compile the distribution silent
# install engine for verification / for the final elevated rebuild chain.
#
# HARD RULE: the DEFAULT output is a fresh temp directory. This script NEVER
# overwrites src\setup\ramboostsetup.exe unless -OutFile explicitly names it
# (only the final elevated distribution-fix chain is allowed to do that).
#
# Compile switches are taken verbatim from src\setup\_rebuild.cmd:
#   cl /nologo /W4 /O2 /utf-8 ramboostsetup.c /Fe:<exe> /link
#      advapi32.lib wintrust.lib crypt32.lib shell32.lib user32.lib
# cl.exe mishandles absolute /Fe paths containing non-ASCII characters (the
# repo path does), so compilation always happens inside src\setup with short
# relative ASCII names; the result is then copied to the requested output.
param(
    [string]$OutFile = ''
)
$ErrorActionPreference = 'Stop'

$setupDir = $PSScriptRoot
$vct      = 'C:\Program1\VC\Tools\MSVC\14.44.35207'
$kit      = 'C:\Program Files (x86)\Windows Kits\10'
$sdkv     = '10.0.26100.0'
$cl       = Join-Path $vct 'bin\Hostx64\x64\cl.exe'
$src      = Join-Path $setupDir 'ramboostsetup.c'

# [88 H-9] fail-closed on every hardcoded toolchain path.
$required = @(
    $vct, $kit, (Join-Path $vct 'include'), (Join-Path $vct 'lib\x64'),
    (Join-Path $kit "Include\$sdkv\ucrt"), (Join-Path $kit "Include\$sdkv\um"),
    (Join-Path $kit "Include\$sdkv\shared"),
    (Join-Path $kit "Lib\$sdkv\um\x64"), (Join-Path $kit "Lib\$sdkv\ucrt\x64")
)
foreach ($p in $required) {
    if (-not (Test-Path -LiteralPath $p)) { throw "toolchain path missing: $p" }
}
if (-not (Test-Path -LiteralPath $cl)) { throw "cl.exe not found: $cl" }
if (-not (Test-Path -LiteralPath $src)) { throw "engine source missing: $src" }

if ([string]::IsNullOrWhiteSpace($OutFile)) {
    $tmpDir = Join-Path $env:TEMP ('rbc-engine-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null
    $OutFile = Join-Path $tmpDir 'ramboostsetup.exe'
} else {
    $OutFile = [IO.Path]::GetFullPath($OutFile)
    $parent = Split-Path -Parent $OutFile
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
}

$env:INCLUDE = "$vct\include;$kit\Include\$sdkv\ucrt;$kit\Include\$sdkv\um;$kit\Include\$sdkv\shared"
$env:LIB     = "$vct\lib\x64;$kit\Lib\$sdkv\um\x64;$kit\Lib\$sdkv\ucrt\x64"
$env:PATH    = "$vct\bin\Hostx64\x64;$env:PATH"

$buildExe = 'rbc_engine_build.exe'
$buildObj = 'rbc_engine_build.obj'
$clArgs = @(
    '/nologo', '/W4', '/O2', '/utf-8', 'ramboostsetup.c',
    "/Fe:$buildExe", "/Fo:$buildObj",
    '/link', 'advapi32.lib', 'wintrust.lib', 'crypt32.lib', 'shell32.lib', 'user32.lib'
)
Push-Location -LiteralPath $setupDir
try {
    Write-Host '== build distribution engine =='
    Write-Host ('  cl     : ' + $cl)
    Write-Host ('  out    : ' + $OutFile)
    Write-Host ('  command: ' + $cl + ' ' + ($clArgs -join ' '))
    & $cl @clArgs
    $rc = $LASTEXITCODE
    Write-Host ('  exit   : ' + $rc)
    if ($rc -ne 0) { throw "engine build failed (exit=$rc)" }
    $built = Join-Path $setupDir $buildExe
    if (-not (Test-Path -LiteralPath $built)) { throw "compiler produced no exe: $built" }
    Copy-Item -LiteralPath $built -Destination $OutFile -Force
} finally {
    Pop-Location
    Remove-Item -LiteralPath (Join-Path $setupDir $buildExe) -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $setupDir $buildObj) -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path -LiteralPath $OutFile)) { throw "engine output missing: $OutFile" }
$h = (Get-FileHash -LiteralPath $OutFile -Algorithm SHA256).Hash.ToLower()
Write-Host ('  sha256 : ' + $h)
Write-Host ('  size   : ' + (Get-Item -LiteralPath $OutFile).Length)
Write-Host ('OUT=' + $OutFile)
Write-Host 'NOTE: default build is verification-only; the signed delivered engine is untouched.'
exit 0
