# use_node27.ps1 -- dot-source this to pin the CURRENT shell to Node 27 + npm 12.2.1.
# Usage (PowerShell):  . .\src\scripts\use_node27.ps1
# Then: node --version ; npm --version ; npm run build
# npm 12.2.1 prints a known false-positive engines warning on Node 27 alpha.
# We set npm_config_loglevel=error so that single warning is suppressed.
$node27 = if ($env:RBC_NODE27) { $env:RBC_NODE27 } else { 'D:\node-v27.0.0-alpha.0.0.0-win-x64' }
if (-not (Test-Path -LiteralPath (Join-Path $node27 'node.exe'))) {
    Write-Host "[X] Node 27 not found at $node27 (set RBC_NODE27 to override)" -ForegroundColor Red
    return
}
$env:PATH = "$node27;$env:PATH"
$env:npm_config_loglevel = 'error'
$nv = & (Join-Path $node27 'node.exe') --version
$np = & (Join-Path $node27 'node.exe') (Join-Path $node27 'node_modules\npm\bin\npm-cli.js') --version --loglevel=error
Write-Host ("[ok] Node 27 shell: node={0} npm={1}" -f $nv, $np) -ForegroundColor Green
Write-Host "     npm alpha engines warning suppressed via npm_config_loglevel=error"
