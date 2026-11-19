# paths.ps1 -- repo path + toolchain constants, dot-sourced by build/build.ps1.
# ASCII only (PS 5.1 reads BOM-less UTF-8 as ANSI; repo path itself is non-ASCII).
#
# Rule: no build script may hardcode a source list, an include dir, or a
# toolchain root. Everything comes from sources.manifest.json through here.

$BuildRoot    = $PSScriptRoot
$RepoRoot     = Split-Path -Parent $PSScriptRoot
$ManifestPath = Join-Path $BuildRoot 'sources.manifest.json'

if (-not (Test-Path -LiteralPath $ManifestPath)) {
    throw "sources.manifest.json missing: $ManifestPath"
}
$Manifest  = [IO.File]::ReadAllText($ManifestPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
$Toolchain = $Manifest.toolchain
$Msvc      = $Toolchain.msvcRoot
$WinSdk    = $Toolchain.winSdkRoot
$SdkVer    = $Toolchain.winSdkVersion
$Wdk       = Join-Path $RepoRoot ($Toolchain.wdkRoot -replace '/', '\')

function Assert-Path([string]$Path, [string]$What) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "$What missing: $Path" }
}

function Get-RepoPath([string]$Relative) {
    return (Join-Path $RepoRoot ($Relative -replace '/', '\'))
}

# Relative path from a repo-relative work dir to a repo-relative target.
# Example: workDir=src/driver, target=src/core -> ..\..\src\core
function Get-RelFrom([string]$WorkDir, [string]$Target) {
    $depth = @($WorkDir -split '/').Count
    return (('..\' * $depth) + ($Target -replace '/', '\'))
}

function Get-BaseName([string]$Path) {
    return (($Path -replace '/', '\').Split('\')[-1])
}

function Get-Stem([string]$Path) {
    $n = Get-BaseName $Path
    return [IO.Path]::GetFileNameWithoutExtension($n)
}

# Toolchain env for the compiler/linker.
#   Mode=user   : SDK ucrt/um/shared (cl.exe for user-mode exes)
#   Mode=kernel : WDK km + SDK shared/ucrt (WDM driver)
function Set-ToolchainEnv([ValidateSet('user', 'kernel')][string]$Mode) {
    Assert-Path $Msvc 'MSVC root'
    Assert-Path $WinSdk 'Windows SDK root'
    Assert-Path (Join-Path $Msvc 'bin\Hostx64\x64\cl.exe') 'cl.exe'

    $sdkInc = Join-Path $WinSdk "Include\$SdkVer"
    $sdkLib = Join-Path $WinSdk "Lib\$SdkVer"
    if ($Mode -eq 'kernel') {
        Assert-Path $Wdk 'WDK package root'
        $wdkInc = Join-Path $Wdk "Include\$SdkVer"
        $wdkLib = Join-Path $Wdk "Lib\$SdkVer\km\x64"
        Assert-Path $wdkInc 'WDK include dir'
        Assert-Path $wdkLib 'WDK kernel lib dir'
        $env:INCLUDE = "$sdkInc\shared;$sdkInc\ucrt;$wdkInc\km;$wdkInc\shared;$Msvc\include"
        $env:LIB     = "$wdkLib;$Msvc\lib\x64"
    }
    else {
        Assert-Path $sdkInc 'SDK include dir'
        Assert-Path $sdkLib 'SDK lib dir'
        $env:INCLUDE = "$Msvc\include;$sdkInc\ucrt;$sdkInc\um;$sdkInc\shared"
        $env:LIB     = "$Msvc\lib\x64;$sdkLib\ucrt\x64;$sdkLib\um\x64"
    }
    $env:PATH = "$Msvc\bin\HostX64\x64;$env:SystemRoot\System32;$env:SystemRoot;$env:PATH"
}

function Write-Sha256([string]$Path) {
    $h = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower()
    $len = (Get-Item -LiteralPath $Path).Length
    Write-Host ("  {0} | size={1} | sha256={2}" -f (Get-BaseName $Path), $len, $h)
    return $h
}

# .NET SDK resolution.
# The machine-wide C:\Program Files\dotnet host has NO sdk\ folder on this host
# (only the runtime), so `dotnet build` there reports "No .NET SDKs were found".
# Prefer any dotnet.exe that actually owns an sdk\ directory next to it.
function Resolve-Dotnet {
    $candidates = @()
    if ($env:DOTNET_ROOT) { $candidates += (Join-Path $env:DOTNET_ROOT 'dotnet.exe') }
    $candidates += (Join-Path $env:USERPROFILE '.dotnet\dotnet.exe')
    $candidates += 'C:\Program Files\dotnet\dotnet.exe'
    foreach ($c in $candidates) {
        if (-not (Test-Path -LiteralPath $c)) { continue }
        $sdkDir = Join-Path (Split-Path -Parent $c) 'sdk'
        if (Test-Path -LiteralPath $sdkDir) { return $c }
    }
    return $null
}

function Set-DotnetEnv {
    $exe = Resolve-Dotnet
    if (-not $exe) {
        throw 'no .NET SDK found (looked for DOTNET_ROOT, %USERPROFILE%\.dotnet, C:\Program Files\dotnet)'
    }
    $root = Split-Path -Parent $exe
    $env:DOTNET_ROOT = $root
    $env:PATH = "$root;$env:PATH"
    return $exe
}

