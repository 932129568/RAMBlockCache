# benchmark.ps1 - diskspd throughput comparison: uncached baseline (passthrough) vs cached (attach).
# Error-handling convention (spec D.3): $ErrorActionPreference='Stop' plus a non-zero exit on
# failure paths. Behavior difference vs before the refactor is limited to failure paths (which
# previously printed an error but still exited 0); the success path is unchanged.
# The 2>$null on the ctl detach/attach calls are probe invocations (detach before baseline /
# attach before load are allowed to fail, e.g. when the volume is not attached), which D.3
# explicitly permits to be silent; the redirections are kept as-is.
param([string]$Letter='F',[int]$SizeGB=4,[string]$Diskspd='diskspd.exe',
      [string]$Ctl='ramboostctl.exe',[string]$AttachArgs='attach',[string]$DetachArgs='detach')
$ErrorActionPreference = 'Stop'
$f="$($Letter)\rb_bench.dat"
fsutil file createnew $f ($SizeGB*1GB) | Out-Null
function Run($t,$o){ & $Diskspd -c0 -d30 "-t$t" "-o$o" -b64K -r -Sh -w0 $f | Select-String 'MB/s|IOPS' | ForEach-Object {"t$t o$o : $_"} }
"== baseline (passthrough) =="; & $Ctl $DetachArgs 2>$null; Run 1 1; Run 8 8
"== cached (attach) =="; & $Ctl $AttachArgs 2>$null; Run 1 1; Run 8 8; Run 8 8
Remove-Item $f -Force
