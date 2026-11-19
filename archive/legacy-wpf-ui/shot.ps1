param(
    [string]$page = "dashboard",
    [string]$out = "shot.png",
    # 可覆盖被截图的 RamBoostFS.Wpf.exe 路径；默认按本脚本所在目录推导（随仓库迁移自动生效）。
    [string]$exePath = ""
)

Add-Type -AssemblyName System.Drawing

# 原硬编码的旧机绝对路径（C:\Users\...\RAMBoostFS-V7\ui\wpf\bin\Debug\net8.0-windows\）已变量化：
# 缺省 = <脚本所在目录>\bin\Debug\net8.0-windows\RamBoostFS.Wpf.exe（与 shot_full.ps1 / shot_v8.ps1 的发布目录模式一致）。
if (-not $exePath) {
    $exePath = Join-Path $PSScriptRoot "bin\Debug\net8.0-windows\RamBoostFS.Wpf.exe"
}
$exe = $exePath

$cs = @'
using System;
using System.Runtime.InteropServices;
using System.Drawing;
public class WinCap {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rectangle r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
Add-Type -TypeDefinition $cs -ReferencedAssemblies System.Drawing

$p = Start-Process -FilePath $exe -ArgumentList "page=$page" -PassThru
Start-Sleep -Seconds 6

$p.Refresh()
$h = $p.MainWindowHandle
[WinCap]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 800

$r = New-Object System.Drawing.Rectangle
[WinCap]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Width; $ht = $r.Height

$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[WinCap]::PrintWindow($h, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

Stop-Process -Id $p.Id -Force
Write-Host "saved $out ($w x $ht)"
