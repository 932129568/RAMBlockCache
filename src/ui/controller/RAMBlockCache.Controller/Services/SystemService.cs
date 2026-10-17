using System;
using System.IO;
using System.Linq;
using System.Security.Principal;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 系统环境辅助：提权检测、盘符 ⇄ 设备键编解码、固定卷枚举、卷标读取、开机启动项。
/// </summary>
public static class SystemService
{
    /// <summary>当前进程是否以管理员运行（IOCTL 需要）。</summary>
    public static bool IsElevated =>
        new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);

    // 真实驱动按盘符识别卷：把盘符 A-Z 编码为稳定 ulong 设备键（替代文档假设的 FNV-GUID）。
    /// <summary>盘符根（如 "C:\"）→ 设备键（盘符 ASCII 值）；无字母返回 0。</summary>
    public static ulong DeviceKeyFromVolumeGuid(string driveOrRoot)
    {
        if (string.IsNullOrEmpty(driveOrRoot)) return 0;
        char c = char.ToUpperInvariant(driveOrRoot.FirstOrDefault(char.IsLetter));
        return c == default ? 0 : (ulong)c;
    }

    /// <summary>设备键 → 盘符（低 16 位）。</summary>
    public static char LetterFromKey(ulong deviceKey) => (char)(ushort)(deviceKey & 0xFFFF);

    /// <summary>枚举固定磁盘卷（盘符根，如 "C:\"）。</summary>
    public static string[] EnumerateFixedVolumes() =>
        DriveInfo.GetDrives()
            .Where(d => d.DriveType == DriveType.Fixed && d.IsReady)
            .Select(d => d.Name)
            .ToArray();

    /// <summary>卷标；取不到时返回"本地磁盘"。</summary>
    public static string LabelFromRoot(string root)
    {
        try
        {
            var di = new DriveInfo(root);
            return !string.IsNullOrEmpty(di.VolumeLabel) ? di.VolumeLabel : "本地磁盘";
        }
        catch { return "本地磁盘"; }
    }

    /// <summary>开/关"随 Windows 启动"（HKCU ...\Run 键 "RAMBlockCache.Controller"）。</summary>
    public static void SetLaunchOnBoot(bool enable)
    {
        const string keyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
        const string name = "RAMBlockCache.Controller";
        using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(keyPath, writable: true)
                        ?? Microsoft.Win32.Registry.CurrentUser.CreateSubKey(keyPath);
        if (enable)
            key.SetValue(name, Environment.ProcessPath ?? "", Microsoft.Win32.RegistryValueKind.String);
        else
            key.DeleteValue(name, throwOnMissingValue: false);
    }
}
