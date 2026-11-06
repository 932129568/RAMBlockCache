using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace RbTempGuard;

/// <summary>
/// 控制面客户端：把排除区间下发给 \\.\RAMBlockCache（IOCTL_RB_EXCLUDE）。
/// ABI 见 ctl/rb_excl.h —— 常量必须与之逐字一致。
/// </summary>
internal static class DriverClient
{
    // CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, 0x808, METHOD_BUFFERED=0, FILE_ANY_ACCESS=0)
    private const uint IOCTL_RB_EXCLUDE = (0x22u << 16) | (0x808u << 2);
    private const int HdrSize = 32;
    private const int EntSize = 16;
    public const int MaxExtents = 1024;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes,
        uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(
        SafeFileHandle hDevice, uint dwIoControlCode,
        byte[] lpInBuffer, int nInBufferSize,
        byte[] lpOutBuffer, int nOutBufferSize,
        out int lpBytesReturned, IntPtr lpOverlapped);

    public static bool TryOpen(out SafeFileHandle h, out string why)
    {
        h = CreateFileW(@"\\.\RAMBlockCache", 0xC0000000u, 0x3u, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h.IsInvalid)
        {
            int e = Marshal.GetLastWin32Error();
            why = e switch
            {
                2 => "驱动未加载（找不到 \\\\.\\RAMBlockCache）",
                5 => "拒绝访问：需要以管理员身份运行",
                _ => "CreateFile 失败 0x" + e.ToString("X")
            };
            h.Dispose();
            h = null;
            return false;
        }
        why = null;
        return true;
    }

    /// <summary>发送排除集（count==0 即清空）。返回 NTSTATUS 或抛 Win32Exception。</summary>
    public static int Send(SafeFileHandle h, char driveLetter, IReadOnlyList<(ulong Start, ulong Len)> ents)
    {
        if (ents.Count > MaxExtents) throw new ArgumentException($"区间数 {ents.Count} > {MaxExtents}");
        int size = HdrSize + ents.Count * EntSize;
        byte[] buf = new byte[size];
        BitConverter.GetBytes((uint)driveLetter).CopyTo(buf, 0);
        BitConverter.GetBytes((uint)ents.Count).CopyTo(buf, 4);
        BitConverter.GetBytes(0u).CopyTo(buf, 8);      // Flags 必须 0
        BitConverter.GetBytes(0u).CopyTo(buf, 12);     // Reserved
        for (int i = 0; i < ents.Count; i++)
        {
            BitConverter.GetBytes(ents[i].Start).CopyTo(buf, HdrSize + i * EntSize);
            BitConverter.GetBytes(ents[i].Len).CopyTo(buf, HdrSize + i * EntSize + 8);
        }
        if (!DeviceIoControl(h, IOCTL_RB_EXCLUDE, buf, size, null, 0, out _, IntPtr.Zero))
        {
            int e = Marshal.GetLastWin32Error();
            // 87=参数错误 1167=设备未就绪 2=找不到设备
            return e switch
            {
                87 => unchecked((int)0xC000000D),
                1167 => unchecked((int)0xC00000A3),
                2 => unchecked((int)0xC0000034),
                _ => unchecked((int)0xC0000001)
            };
        }
        return 0;
    }
}
