$ErrorActionPreference = "Continue"
Add-Type -AssemblyName System.Drawing

$pub = Join-Path $PSScriptRoot "bin\Release\net8.0-windows\publish"

$cs = @'
using System;
using System.Runtime.InteropServices;
using System.Drawing;
public class WinCap {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rectangle r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
'@
Add-Type -TypeDefinition $cs -ReferencedAssemblies System.Drawing

$exe = Join-Path $pub "RamBoostFS.Wpf.exe"
$shotDir = Join-Path $pub "shots"
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null

$HWND_TOPMOST = [IntPtr]::Zero - 1
$SWP_NOMOVE = 0x0002; $SWP_NOSIZE = 0x0001; $SWP_SHOWWINDOW = 0x0040

$pages = @("dashboard","memory","volumes","security","logs")

foreach ($page in $pages) {
    $p = Start-Process -FilePath $exe -ArgumentList "page=$page" -PassThru
    Start-Sleep -Seconds 8

    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { Write-Host "[$page] no window handle"; taskkill /F /PID $p.Id 2>$null; continue }

    [WinCap]::ShowWindow($h, 9) | Out-Null
    [WinCap]::SetWindowPos($h, $HWND_TOPMOST, 0,0,0,0, $SWP_NOMOVE -bor $SWP_NOSIZE -bor $SWP_SHOWWINDOW) | Out-Null
    [WinCap]::SetForegroundWindow($h) | Out-Null
    [WinCap]::BringWindowToTop($h) | Out-Null
    Start-Sleep -Milliseconds 1500

    $r = New-Object System.Drawing.Rectangle
    [WinCap]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Width; $ht = $r.Height

    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Location, [System.Drawing.Point]::Empty, $r.Size)
    $out = Join-Path $shotDir ("{0}.png" -f $page)
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()

    Write-Host "[$page] saved $out ($w x $ht)"
    taskkill /F /PID $p.Id 2>&1 | Out-Null
    Start-Sleep -Milliseconds 1200
}

Write-Host "=== selfcheck.log ==="
$sc = Join-Path $pub "selfcheck.log"
if (Test-Path $sc) { Get-Content $sc -Encoding UTF8 }
Write-Host "=== crash.log ==="
$cl = Join-Path $pub "crash.log"
if (Test-Path $cl) { Get-Content $cl -Encoding UTF8 } else { Write-Host "no crash.log (good)" }
