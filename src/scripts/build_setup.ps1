# build_setup.ps1 -- single-file installer build chain (U0-2 v2)
# One command produces dist\RAMBlockCache-Setup.exe (double-click install, no unpack):
#   1) publish WPF controller (framework-dependent Release)
#   2) refresh Assets/payload (driver / silent engine / controller / CLI / INF / README)
#   3) publish single-file setup (all payload embedded)
# Discipline: pure ASCII (GBK console pitfall); every step checks exit code, fail fast.
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$setup = Join-Path $root "src\ui\installer\RAMBlockCache.Setup"
$ctl   = Join-Path $root "src\ui\controller\RAMBlockCache.Controller"
$pay   = Join-Path $setup "Assets\payload"
$dotnet = "C:\Users\陈\.dotnet\dotnet.exe"
if (-not (Test-Path $dotnet)) { $dotnet = "dotnet" }

Write-Host "== [1/3] publish controller =="
& $dotnet publish $ctl -c Release -v q
if ($LASTEXITCODE -ne 0) { throw "controller publish failed (exit=$LASTEXITCODE)" }
$ctlPub = Join-Path $ctl "bin\Release\net8.0-windows\publish"

Write-Host "== [2/3] refresh payload =="
if (Test-Path $pay) { Remove-Item $pay -Recurse -Force }
New-Item -ItemType Directory -Force -Path $pay | Out-Null
# kernel side (staging consumer; engine and sys must sit side by side)
Copy-Item (Join-Path $root "dist\RAMBlockCache.sys")        $pay -Force   # signed driver (same anchor source)
Copy-Item (Join-Path $root "src\setup\ramboostsetup.exe")   $pay -Force   # silent engine (signature fail-closed inside)
# app side (lands in install dir)
$appFiles = @(
    "RAMBlockCache.Controller.exe",
    "RAMBlockCache.Controller.dll",
    "RAMBlockCache.Controller.runtimeconfig.json",
    "RAMBlockCache.Controller.deps.json"
)
foreach ($f in $appFiles) {
    Copy-Item (Join-Path $ctlPub $f) $pay -Force
}
Copy-Item (Join-Path $root "src\ctl\ramboostctl.exe")  $pay -Force
Copy-Item (Join-Path $root "src\ctl\rbfeatctl.exe")    $pay -Force
Copy-Item (Join-Path $root "src\driver\RAMBlockCache.inf") $pay -Force
Copy-Item (Join-Path $root "README.txt")               $pay -Force
Get-ChildItem $pay | ForEach-Object { Write-Host ("  payload: {0}  {1} KB" -f $_.Name, [int]($_.Length/1KB)) }

Write-Host "== [3/3] publish single-file setup =="
& $dotnet publish $setup -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -v q
if ($LASTEXITCODE -ne 0) { throw "setup publish failed (exit=$LASTEXITCODE)" }
$out = Join-Path $setup "bin\Release\net8.0-windows\win-x64\publish\RAMBlockCache-Setup.exe"
if (-not (Test-Path $out)) { throw "published exe not found: $out" }
$dest = Join-Path $root "dist\RAMBlockCache-Setup.exe"
Copy-Item $out $dest -Force
$h = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
Write-Host "=== BUILD OK ==="
Get-Item -LiteralPath $dest | Select-Object FullName, Length, LastWriteTime | Format-List
Write-Host "SHA256 = $h"
Write-Host "NOTE: the embedded dist\RAMBlockCache.sys is the signed build; if the driver is not rebuilt the deploy anchors stay valid."
Write-Host "NOTE: running requires .NET 8 Desktop Runtime (same bar as the controller); UAC elevates automatically (app.manifest)."
