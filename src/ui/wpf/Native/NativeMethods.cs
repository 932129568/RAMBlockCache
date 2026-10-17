using System;
using System.Runtime.InteropServices;

namespace RamBoostFS.Wpf.Native;

/// <summary>
/// Win32 P/Invoke 声明集合（internal）：块层控制设备 I/O、DWM 窗口属性、
/// 物理内存计数、卷空间查询、ShellExecuteExW（提权 bcdedit/脚本）。
/// </summary>
internal static class NativeMethods
{
    /* ---------- V8 块层控制设备: CreateFile + DeviceIoControl ---------- */
    /* 打开 \\.\RAMBlockCache, 用单个 IOCTL_RB_REQUEST (METHOD_BUFFERED) 收发 RB_MSG。
     * 对齐 ctl/rb_proto.h: 无 FILE_READ/WRITE_DATA 限制, dwDesiredAccess=0, FILE_ANY_ACCESS。 */

    internal const uint GENERIC_READ = 0x80000000;
    internal const uint GENERIC_WRITE = 0x40000000;
    internal const uint OPEN_EXISTING = 3;
    internal static readonly IntPtr INVALID_HANDLE_VALUE = new(-1);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    internal static extern IntPtr CreateFileW(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool DeviceIoControl(
        IntPtr hDevice,
        uint dwIoControlCode,
        IntPtr lpInBuffer,
        uint nInBufferSize,
        IntPtr lpOutBuffer,
        uint nOutBufferSize,
        out uint lpBytesReturned,
        IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool CloseHandle(IntPtr hObject);

    /* ---------- DWM 亚克力/Mica ---------- */

    internal const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    internal const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
    internal const int DWMWA_CORNER_PREFERENCE = 33;
    internal const int DWMWCP_ROUND = 2;
    internal const int DWMSBT_MAINWINDOW_MICA = 2;

    [DllImport("dwmapi.dll", PreserveSig = true)]
    internal static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int pv, int cb);

    /* ---------- 物理内存计数 ---------- */

    [StructLayout(LayoutKind.Sequential)]
    internal struct MEMORYSTATUSEX
    {
        public uint dwLength;
        public uint dwMemoryLoad;
        public ulong ullTotalPhys;
        public ulong ullAvailPhys;
        public ulong ullTotalPageFile;
        public ulong ullAvailPageFile;
        public ulong ullTotalVirtual;
        public ulong ullAvailVirtual;
        public ulong ullAvailExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX lpBuffer);

    /* ---------- 卷枚举 (kernel32) ---------- */

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool GetDiskFreeSpaceExW(
        string lpDirectoryName,
        out ulong lpFreeBytesAvailable,
        out ulong lpTotalNumberOfBytes,
        out ulong lpTotalNumberOfFreeBytes);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    internal static extern uint GetLogicalDrives();

    /* ---------- bcdedit 游戏模式 (需提权) ---------- */

    [StructLayout(LayoutKind.Sequential)]
    internal struct SHELLEXECUTEINFO
    {
        public int cbSize;
        public uint fMask;
        public IntPtr hwnd;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpVerb;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpFile;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpParameters;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpDirectory;
        public int nShow;
        public IntPtr hInstApp;
        public IntPtr lpIDList;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpClass;
        public IntPtr hkeyClass;
        public uint dwHotKey;
        public IntPtr hIcon;
        public IntPtr hProcess;
    }

    internal const uint SEE_MASK_NOCLOSEPROCESS = 0x00000040;

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool ShellExecuteExW(ref SHELLEXECUTEINFO lpExecInfo);
}
