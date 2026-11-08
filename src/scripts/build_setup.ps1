# build_setup.ps1 -- single-file installer build chain (U0-2 v2)
# One command produces dist\RAMBlockCache-Setup.exe (double-click install, no unpack):
#   1) publish WPF controller (SELF-CONTAINED win-x64: .NET 8 Desktop Runtime embedded)
#   2) refresh Assets/payload (driver / silent engine / controller.zip / webconsole.zip / CLI / INF / README)
#   3) publish single-file setup (all payload embedded)
# Discipline: pure ASCII (GBK console pitfall); every step checks exit code, fail fast.
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$setup = Join-Path $root "src\ui\installer\RAMBlockCache.Setup"
$ctl   = Join-Path $root "src\ui\controller\RAMBlockCache.Controller"
$pay   = Join-Path $setup "Assets\payload"
$dotnet = "C:\Users\陈\.dotnet\dotnet.exe"
if (-not (Test-Path $dotnet)) { $dotnet = "dotnet" }

# ---- [NODE27 2026-11-06] toolchain contract (fail-closed) ------------------
# The shipped portable runtime MUST be Node 27 (user's D:\ install) and the SPA
# MUST be built with npm 12.2.1. The old line used (Get-Command node.exe).Source,
# which silently picked WorkBuddy's Node 22 depending on PATH order.
# npm 12.2.1 prints a KNOWN FALSE-POSITIVE engines warning on Node 27 alpha:
# its own engines says ">=26.0.0", and semver excludes 27.0.0-alpha prereleases.
# Functional check: npm ci + npm run build both PASS; we suppress that single
# warning with --loglevel=error at every npm entry point (user decision 15:14).
function Resolve-Node27 {
    $cands = @()
    if ($env:RBC_NODE27) { $cands += $env:RBC_NODE27 }
    $cands += 'D:\node-v27.0.0-alpha.0.0.0-win-x64\node.exe'
    $cands += (Join-Path $env:USERPROFILE '.workbuddy\binaries\node\versions\node-v27.0.0-alpha.0.0.0-win-x64\node.exe')
    try { $cands += (Get-Command node.exe -ErrorAction Stop).Source } catch { }
    foreach ($c in $cands) {
        if (-not $c -or -not (Test-Path -LiteralPath $c)) { continue }
        $v = (& $c --version 2>$null)
        if ($LASTEXITCODE -ne 0 -or "$v" -notlike 'v27.*') { continue }
        $dir = Split-Path -Parent $c
        $npmCli = Join-Path $dir 'node_modules\npm\bin\npm-cli.js'
        if (-not (Test-Path -LiteralPath $npmCli)) { throw "npm-cli.js missing next to $c" }
        $np = (& $c $npmCli --version --loglevel=error 2>$null | Select-Object -Last 1)
        if ("$np" -notlike '12.2.1*') { throw "npm is '$np', expected 12.2.1 (Node 27 toolchain)" }
        Write-Host ("  node27: {0}  ({1})" -f $v, $c)
        Write-Host ("  npm   : {0}  (known alpha engines warning suppressed)" -f $np)
        return @{ Node = $c; NpmCli = $npmCli; Dir = $dir }
    }
    throw "Node 27 + npm 12.2.1 not found. Set RBC_NODE27=<path to node.exe>; refusing to package a Node 22 runtime."
}
$tool = Resolve-Node27

Write-Host "== [1/4] publish controller (self-contained) =="
& $dotnet publish $ctl -c Release -r win-x64 --self-contained true -v q
if ($LASTEXITCODE -ne 0) { throw "controller publish failed (exit=$LASTEXITCODE)" }
# [PACK 2026-11-04] self-contained = .NET 8 Desktop Runtime embedded => zero framework
# dependency on target machines ("install and run"). Directory zipped because Payload
# embeds flat names only (see Services/Payload.cs).
$ctlPub = Join-Path $ctl "bin\Release\net10.0-windows\win-x64\publish"

Write-Host "== [2/4] build SPA + stage webconsole (Node 27 + npm 12.2.1) =="
$webRoot = Join-Path $root "web"
# [NODE27] SPA is always rebuilt with the pinned toolchain before packaging;
# missing node_modules -> bootstrap with npm ci (same pinned npm).
Push-Location $webRoot
if (-not (Test-Path (Join-Path $webRoot 'node_modules'))) {
    & $tool.Node $tool.NpmCli ci --no-audit --no-fund --loglevel=error
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "npm ci failed (exit=$LASTEXITCODE)" }
}
& $tool.Node $tool.NpmCli run build --loglevel=error
$rcWeb = $LASTEXITCODE
Pop-Location
if ($rcWeb -ne 0) { throw "web build failed (exit=$rcWeb) - refusing to package stale SPA" }
$wcStage = Join-Path $env:TEMP ("rbc-webconsole-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $wcStage | Out-Null
Copy-Item (Join-Path $webRoot "bridge") (Join-Path $wcStage "bridge") -Recurse -Force
Copy-Item (Join-Path $webRoot "dist")   (Join-Path $wcStage "dist")   -Recurse -Force
Copy-Item $tool.Node $wcStage -Force
# [NODE27] "all built in": ship npm 12.2.1 alongside the portable node.exe, so a
# target machine needs NO pre-installed Node/npm. npm.cmd shim resolves node.exe
# from its own directory, so the pair is fully relocatable.
$npmSrc = Join-Path $tool.Dir 'node_modules\npm'
$npmDst = Join-Path $wcStage 'node_modules\npm'
if (-not (Test-Path -LiteralPath (Split-Path -Parent $npmDst))) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $npmDst) | Out-Null
}
Copy-Item -LiteralPath $npmSrc -Destination $npmDst -Recurse -Force
foreach ($shim in @('npm.cmd','npm.ps1','npm','npx.cmd','npx.ps1','npx')) {
    $s = Join-Path $tool.Dir $shim
    if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination $wcStage -Force }
}
# [105 BUGFIX] bridge runtime deps: bridge/server/index.ts imports hono /
# @hono/node-server / pino / zod — resolved at runtime by walking UP to
# <repo>/web/node_modules (the standalone webconsole layout has no parent node_modules)
# node_modules, so the zip must CARRY the full production closure. Compute it
# with the same walker Node uses (recursive package.json dependencies from the
# 4 roots), copy each package dir into wcStage/node_modules. Never hardcode
# the list (it drifts); missing packages abort the build (fail-closed).
Push-Location $webRoot
$bridgeClosure = (& $tool.Node -e "const fs=require('fs'),path=require('path');const roots=['hono','@hono/node-server','pino','zod'];const seen=new Set();const q=[...roots];while(q.length){const n=q.shift();if(seen.has(n))continue;seen.add(n);const pj=path.join('node_modules',n,'package.json');if(!fs.existsSync(pj)){console.error('MISSING '+n);process.exit(3);}const j=JSON.parse(fs.readFileSync(pj,'utf8'));for(const d of Object.keys(j.dependencies||{}))if(!seen.has(d))q.push(d);}console.log(Array.from(seen).sort().join(String.fromCharCode(10)));")
Pop-Location
if ($LASTEXITCODE -ne 0 -or -not $bridgeClosure) { throw "bridge dep closure walk failed (exit=$LASTEXITCODE)" }
$copied = 0
foreach ($name in ($bridgeClosure -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    $srcPkg = Join-Path $webRoot ("node_modules" + [IO.Path]::DirectorySeparatorChar + $name.Replace('/', [IO.Path]::DirectorySeparatorChar))
    $dstPkg = Join-Path $wcStage ("node_modules" + [IO.Path]::DirectorySeparatorChar + $name.Replace('/', [IO.Path]::DirectorySeparatorChar))
    if (-not (Test-Path -LiteralPath $srcPkg)) { throw "bridge dep missing on disk: $name" }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dstPkg) | Out-Null
    Copy-Item -LiteralPath $srcPkg -Destination $dstPkg -Recurse -Force
    $copied++
}
Write-Host ("  payload: bridge runtime deps copied ({0} packages: hono/@hono/pino/zod closure)" -f $copied)
# [NODE27] strip dev-only artifacts from the staged bridge (~100MB of Rust
# target/ + .mimosa logs). Runtime needs only native/build/Release/*.node.
Get-ChildItem -LiteralPath (Join-Path $wcStage 'bridge') -Recurse -Directory -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -in @('.mimosa','target','.cargo','.git') } |
    Sort-Object { $_.FullName.Length } -Descending |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host "  payload: portable Node 27 + npm 12.2.1 built in (zero target dependency)"

Write-Host "== [3/4] refresh payload =="
# [NODE27 2026-11-06] refresh by OVERWRITE, no recursive delete. Some hosts
# (safe-delete hooks / AV) block Remove-Item -Recurse on this tree and abort the
# whole build. Payload.cs extracts a fixed file list, so leftover files are
# inert; every artifact below is written with -Force / Compress-Archive -Force.
New-Item -ItemType Directory -Force -Path $pay | Out-Null
# kernel side (staging consumer; engine and sys must sit side by side)
Copy-Item (Join-Path $root "dist\RAMBlockCache.sys")      $pay -Force   # signed driver (same anchor source)
Copy-Item (Join-Path $root "src\setup\ramboostsetup.exe") $pay -Force   # silent engine (signature fail-closed inside)
# [DISTRIB] 根证书必须随包：引擎在"根不受信任"时自动导入，否则新机器装不上
# （实测症状：嵌入式载荷缺失 RAMBoostFS-Root.cer —— 安装直接失败）。
$cerSrc = Join-Path $root "certs\RAMBoostFS-Root.cer"
if (-not (Test-Path -LiteralPath $cerSrc)) { $cerSrc = Join-Path $root "src\setup\RAMBoostFS-Root.cer" }
if (-not (Test-Path -LiteralPath $cerSrc)) { throw "root cert not found (expected certs\RAMBoostFS-Root.cer)" }
Copy-Item -LiteralPath $cerSrc $pay -Force
Write-Host "  payload: RAMBoostFS-Root.cer (from $cerSrc)"
# controller.zip (self-contained; extracted to install dir root by SetupEngine)
$ctlZip = Join-Path $pay "controller.zip"
Compress-Archive -Path (Join-Path $ctlPub '*') -DestinationPath $ctlZip -Force
Write-Host ("  payload: controller.zip  {0} KB" -f [int]((Get-Item $ctlZip).Length/1KB))
# webconsole.zip (portable node + bridge + dist; extracted to install dir\webconsole)
$wcZip = Join-Path $pay "webconsole.zip"
Compress-Archive -Path (Join-Path $wcStage '*') -DestinationPath $wcZip -Force
Write-Host ("  payload: webconsole.zip  {0} KB" -f [int]((Get-Item $wcZip).Length/1KB))
# [FIX 2026-11-06 22:5x] This staging dir is a fresh GUID folder under %TEMP%
# and webconsole.zip is already complete. A safe-delete hook / AV can block a
# recursive delete in an ELEVATED session (15:33 UAC interruption); with
# $ErrorActionPreference='Stop' that abort would kill the whole build. The
# 73-doc full-chain replay wrapper is ONE-SHOT, so a mid-run abort is costly.
# Cleanup is best-effort only; leftover files are inert (unique path).
try { Remove-Item -LiteralPath $wcStage -Recurse -Force -ErrorAction Stop }
catch { Write-Host ("  [warn] staging cleanup skipped (harmless): " + $_.Exception.Message) }
# web console launcher (installed alongside; shortcut points here).
# [FIX 2026-11-06] Entry MUST be bridge\server\index.ts to match webconsole.zip:
# zip top-level = bridge/ + dist/ + node.exe, there is NO server/ directory.
# cwd is %~dp0webconsole, so the relative path resolves to
# <InstallDir>\webconsole\bridge\server\index.ts.
# [88 L-1 2026-11-07] The old "same as MainWindow.xaml.cs" note was stale:
# the bridge now resolves dist/ from import.meta.url, so a scheduled task's
# default cwd no longer matters; only the entry path above must match the zip.
$startCmd = "@echo off`r`n" +
            "rem RAMBlockCache Web Console launcher (installed by Setup)`r`n" +
            'cd /d "%~dp0webconsole"' + "`r`n" +
            'if not exist "node.exe" ( echo [x] node.exe missing & pause & exit /b 1 )' + "`r`n" +
            'start "" /b "node.exe" "bridge\server\index.ts"' + "`r`n" +
            "timeout /t 3 /nobreak >nul" + "`r`n" +
            "start http://127.0.0.1:5199/" + "`r`n"
Set-Content -Path (Join-Path $pay "start-webconsole.cmd") -Value $startCmd -Encoding ASCII
# CLI / INF / README / offline rollback
Copy-Item (Join-Path $root "src\ctl\ramboostctl.exe")      $pay -Force
Copy-Item (Join-Path $root "src\ctl\rbfeatctl.exe")        $pay -Force
Copy-Item (Join-Path $root "src\driver\RAMBlockCache.inf") $pay -Force
Copy-Item (Join-Path $root "README.txt")                   $pay -Force
# [FIX 2026-11-06] 0x7B one-click offline rollback ships with the product now
# (previously only referenced as src\scripts\... which does not exist on targets).
# MUST stay pure ASCII + CRLF (cmd parsing discipline C1).
Copy-Item (Join-Path $root "src\scripts\offline_rollback.cmd") $pay -Force
# [P1.5 2026-11-06] boot_guard 看门脚本随包：引擎安装时复制到 ProgramData\RAMBoost 并武装双计划任务
Copy-Item (Join-Path $root "src\scripts\boot_guard.ps1")   $pay -Force
Copy-Item (Join-Path $root "src\scripts\logon_disarm.ps1") $pay -Force
Get-ChildItem $pay | ForEach-Object { Write-Host ("  payload: {0}  {1} KB" -f $_.Name, [int]($_.Length/1KB)) }

Write-Host "== [4/4] publish single-file setup (self-contained) =="
# [PACK-FIX 2026-11-04] 安装器本体也必须是 self-contained：原先 false 让 Setup.exe
# 自己要求目标机装 .NET 8 Desktop Runtime（实测报 "You must install or update .NET"），
# 用户根本无法开始安装。改 true 后 Setup 自带运行时，双击即装。
& $dotnet publish $setup -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -v q
if ($LASTEXITCODE -ne 0) { throw "setup publish failed (exit=$LASTEXITCODE)" }
# TFM 随 csproj（net8.0-windows / net10.0-windows）变化：自动探测实际输出目录
$out = Get-ChildItem (Join-Path $setup "bin\Release") -Recurse -Filter 'RAMBlockCache-Setup.exe' -ErrorAction SilentlyContinue |
       Where-Object { $_.FullName -match 'win-x64\\publish' } |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $out) { throw "published exe not found under bin\Release\*\win-x64\publish" }
$dest = Join-Path $root "dist\RAMBlockCache-Setup.exe"
Copy-Item $out $dest -Force
$h = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
Write-Host "=== BUILD OK ==="
Get-Item -LiteralPath $dest | Select-Object FullName, Length, LastWriteTime | Format-List
Write-Host "SHA256 = $h"
Write-Host "NOTE: the embedded dist\RAMBlockCache.sys is the signed build; if the driver is not rebuilt the deploy anchors stay valid."
Write-Host "NOTE: controller is SELF-CONTAINED (.NET 8 Desktop Runtime embedded) - no runtime install needed on target."
Write-Host "NOTE: webconsole ships portable node.exe - the Web console is one click via start-webconsole.cmd."
