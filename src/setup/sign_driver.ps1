# Reusable driver signing helper (run elevated). Signs with the local RAMBoostFS root.
param(
    [Parameter(Mandatory=$true)][string]$Target,
    [string]$Thumbprint = 'DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431',
    [string]$ResultFile = ''
)
$sign = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$lines = @()
if(-not (Test-Path $Target)){ $lines += "TARGET NOT FOUND: $Target" }
else{
    $lines += & $sign sign /sm /s My /sha1 $Thumbprint /fd SHA256 $Target 2>&1
    $lines += "SIGN_EXIT=$LASTEXITCODE"
}
$out = if($ResultFile){ $ResultFile } else { Join-Path $env:TEMP 'rbsign_result.txt' }
$lines | Set-Content -Encoding UTF8 -Path $out
