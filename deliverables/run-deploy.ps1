# run-deploy.ps1 -- wrapper that actually captures deploy.ps1 output to a file.
# Written as a FILE on purpose: passing a compound command through Start-Process
# -ArgumentList mangles quotes, which silently produced an empty log and a
# meaningless exit code on the first attempt.
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot   # deliverables\ -> repo root
$Deploy = Join-Path $Root 'src\scripts\deploy.ps1'
$Log = Join-Path $env:TEMP 'deploy-out.log'

"=== wrapper start $(Get-Date -Format o) ===" | Out-File -LiteralPath $Log -Encoding utf8
"elevated: $([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))" |
    Out-File -LiteralPath $Log -Append -Encoding utf8
"deploy script: $Deploy" | Out-File -LiteralPath $Log -Append -Encoding utf8

if (-not (Test-Path -LiteralPath $Deploy)) {
    "FATAL: deploy.ps1 not found" | Out-File -LiteralPath $Log -Append -Encoding utf8
    exit 2
}

& powershell -NoProfile -ExecutionPolicy Bypass -File $Deploy *>&1 |
    Tee-Object -FilePath $Log -Append |
    ForEach-Object { Write-Host $_ }

$rc = $LASTEXITCODE
"=== deploy.ps1 exit=$rc ===" | Out-File -LiteralPath $Log -Append -Encoding utf8
exit $rc
