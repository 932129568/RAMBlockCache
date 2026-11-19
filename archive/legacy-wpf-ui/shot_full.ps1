Add-Type -AssemblyName System.Drawing
$pub = Join-Path $PSScriptRoot "bin\Release\net8.0-windows\publish"
$exe = Join-Path $pub "RamBoostFS.Wpf.exe"
$p = Start-Process -FilePath $exe -ArgumentList "page=volumes" -PassThru
Start-Sleep -Seconds 8
$p.Refresh(); $h = $p.MainWindowHandle

Add-Type -AssemblyName System.Windows.Forms
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$out = Join-Path $pub "shots\_fullscreen.png"
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $out virtual=$($b.Width)x$($b.Height) hwnd=$h"
taskkill /F /PID $p.Id 2>&1 | Out-Null
