using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using RamBoostFS.Wpf.Models;

namespace RamBoostFS.Wpf.Native;

/// <summary>
/// V8 主通道: CreateFileW("\\.\RAMBlockCache") 打开块层控制设备, 用单个
/// DeviceIoControl(IOCTL_RB_REQUEST = 0x222000, METHOD_BUFFERED) 收发 RB_MSG。
/// 对齐 ctl/rb_proto.h: Type@0/Size@4/Status(int)@8/Reserved@12/联合体 Data@16。
/// payload 起始偏移是 16 (V7 为 8, 已改)。连不上时 IsConnected=false 且不崩。
/// </summary>
public sealed class RbClient : IRbClient, IDisposable
{
    private IntPtr _hDevice = NativeMethods.INVALID_HANDLE_VALUE;
    private readonly object _lock = new();

    public bool IsConnected => _hDevice != NativeMethods.INVALID_HANDLE_VALUE && _hDevice != IntPtr.Zero;
    public string BackendName => IsConnected
        ? $"DeviceIoControl → {RbProtoDefs.RB_DEVICE_PATH} (IOCTL 0x{RbProtoDefs.IOCTL_RB_REQUEST:X})"
        : "未连接 \\.\\RAMBlockCache";

    public RbClient()
    {
        TryConnect();
    }

    private void TryConnect()
    {
        lock (_lock)
        {
            if (IsConnected) return;
            // dwDesiredAccess=0: 控制设备用 FILE_ANY_ACCESS, 无需 GENERIC_READ/WRITE。
            IntPtr h = NativeMethods.CreateFileW(
                RbProtoDefs.RB_DEVICE_PATH,
                0,
                0,
                IntPtr.Zero,
                NativeMethods.OPEN_EXISTING,
                0,
                IntPtr.Zero);
            if (h == NativeMethods.INVALID_HANDLE_VALUE || h == IntPtr.Zero)
            {
                int err = Marshal.GetLastWin32Error();
                _hDevice = NativeMethods.INVALID_HANDLE_VALUE;
                System.Diagnostics.Debug.WriteLine($"CreateFileW({RbProtoDefs.RB_DEVICE_PATH}) 失败: Win32Error={err}");
            }
            else
            {
                _hDevice = h;
            }
        }
    }

    private void EnsureConnected()
    {
        if (!IsConnected) TryConnect();
        if (!IsConnected) throw new InvalidOperationException("driver device not connected");
    }

    // --- 低层: 写头到 buf, payload 到 buf+16 ---

    private static void StructureToPayload(IntPtr dst, uint type, object payload)
    {
        switch (type)
        {
            case RbProtoDefs.RB_MSG_SET_CONFIG:
                var cfg = (RbConfig)payload;
                RbMarshal.NormalizeConfig(ref cfg);
                Marshal.StructureToPtr(cfg, dst, false);
                break;
            case RbProtoDefs.RB_MSG_SET_VOLUME:
                Marshal.StructureToPtr((RbVolumeSet)payload, dst, false);
                break;
            case RbProtoDefs.RB_MSG_ATTACH_VOLUME:
                Marshal.StructureToPtr((RbVolumeAttach)payload, dst, false);
                break;
            case RbProtoDefs.RB_MSG_GET_LOG:
                var req = (RbLogList)payload;
                RbMarshal.NormalizeLogList(ref req);
                Marshal.StructureToPtr(req, dst, false);
                break;
            default:
                // GET_STATS / GET_CONFIG / FLUSH / EXIT_SAFE_MODE / GET_VERSION 无请求 payload
                break;
        }
    }

    // --- 对外 API ---

    public Task<RbStats?> GetStatsAsync() => Task.Run(() =>
    {
        try { return Roundtrip<RbStats>(RbProtoDefs.RB_MSG_GET_STATS, null); }
        catch { return null; }
    });

    public Task<RbConfig?> GetConfigAsync() => Task.Run(() =>
    {
        try { return Roundtrip<RbConfig>(RbProtoDefs.RB_MSG_GET_CONFIG, null); }
        catch { return null; }
    });

    public Task<bool> SetConfigAsync(RbConfig cfg) => Task.Run(() =>
    {
        try { SendAck(RbProtoDefs.RB_MSG_SET_CONFIG, cfg); return true; }
        catch { return false; }
    });

    public Task<bool> FlushAsync() => Task.Run(() =>
    {
        try { SendAck(RbProtoDefs.RB_MSG_FLUSH, null); return true; }
        catch { return false; }
    });

    public Task<bool> ExitSafeModeAsync() => Task.Run(() =>
    {
        try { SendAck(RbProtoDefs.RB_MSG_EXIT_SAFE_MODE, null); return true; }
        catch { return false; }
    });

    public Task<bool> SetVolumeAsync(char letter, bool read, bool write, uint writeMode) => Task.Run(() =>
    {
        try
        {
            var vs = new RbVolumeSet
            {
                Letter = (ushort)char.ToUpperInvariant(letter),
                ReadEnabled = read ? 1u : 0u,
                WriteEnabled = write ? 1u : 0u,
                WriteMode = writeMode
            };
            SendAck(RbProtoDefs.RB_MSG_SET_VOLUME, vs);
            return true;
        }
        catch { return false; }
    });

    public Task<RbVolumeList?> GetVolumesAsync() => Task.Run(() =>
    {
        try { return Roundtrip<RbVolumeList>(RbProtoDefs.RB_MSG_GET_VOLUMES, null); }
        catch { return null; }
    });

    public Task<bool> AttachVolumeAsync(char letter, bool attach, bool read, bool write, uint writeMode) => Task.Run(() =>
    {
        try
        {
            var va = new RbVolumeAttach
            {
                Letter = (ushort)char.ToUpperInvariant(letter),
                Attach = attach ? 1u : 0u,
                ReadEnabled = read ? 1u : 0u,
                WriteEnabled = write ? 1u : 0u,
                WriteMode = writeMode
            };
            SendAck(RbProtoDefs.RB_MSG_ATTACH_VOLUME, va);
            return true;
        }
        catch { return false; }
    });

    public Task<RbLogList?> GetLogListAsync(uint cursor) => Task.Run(() =>
    {
        // FIX[P20]: 请求只需 cursor（驱动不读 payload），不再 marshal 整个含
        // 64×嵌套 ByValArray 的 RbLogList（~7.7KB 浪费 + 嵌套数组风险）。
        try { return Roundtrip<RbLogList>(RbProtoDefs.RB_MSG_GET_LOG, null); }
        catch { return null; }
    });

    public Task<RbVersion?> GetVersionAsync() => Task.Run(() =>
    {
        try { return Roundtrip<RbVersion>(RbProtoDefs.RB_MSG_GET_VERSION, null); }
        catch { return null; }
    });

    // --- 底层收发 ---

    // 同一个 METHOD_BUFFERED 缓冲区: I/O 管理器为 in/out 提供同一系统缓冲。
    private byte[]? _zeroScratch;   // FIX[P7]: 复用清零缓冲，替代逐字节 7704 次写

    private IntPtr IoRoundtrip(uint type, object? payload)
    {
        lock (_lock)
        {
            EnsureConnected();

            int size = RbProtoDefs.RbMsgSize;
            IntPtr buf = Marshal.AllocHGlobal(size);
            try
            {
                if (_zeroScratch == null) _zeroScratch = new byte[size];
                Marshal.Copy(_zeroScratch, 0, buf, size);       // 一次性清零
                Marshal.WriteInt32(buf, 0, (int)type);          // Type
                Marshal.WriteInt32(buf, 4, size);               // Size = sizeof(RB_MSG)
                // Status@8 / Reserved@12 已由清零覆盖, 驱动应答回填 Status@8
                if (payload != null)
                    StructureToPayload(buf + RbProtoDefs.RbMsgDataOffset, type, payload);

                bool ok = NativeMethods.DeviceIoControl(
                    _hDevice,
                    RbProtoDefs.IOCTL_RB_REQUEST,
                    buf, (uint)size,     // in buffer
                    buf, (uint)size,     // out buffer (同一系统缓冲)
                    out _bytesReturned,
                    IntPtr.Zero);
                if (!ok)
                {
                    int err = Marshal.GetLastWin32Error();
                    // 设备断开/驱动卸载: 关句柄, 下次自动重连; 不抛到 UI 崩程序。
                    NativeMethods.CloseHandle(_hDevice);
                    _hDevice = NativeMethods.INVALID_HANDLE_VALUE;
                    // FIX[P7]: 这里绝不能再手动 FreeHGlobal —— finally 会释放。
                    // 原代码“先 Free 再 throw”，finally 再 Free 一次 => 双重释放、堆损坏。
                    throw new InvalidOperationException(
                        $"DeviceIoControl 0x{RbProtoDefs.IOCTL_RB_REQUEST:X} 失败: Win32Error={err}");
                }

                // 所有权转移给调用方 (SendAck / Roundtrip 负责 FreeHGlobal)
                IntPtr result = buf;
                buf = IntPtr.Zero;
                return result;
            }
            finally
            {
                if (buf != IntPtr.Zero) Marshal.FreeHGlobal(buf);
            }
        }
    }

    private uint _bytesReturned;

    // ack-only：仍须解析驱动回填的 Status@8（NTSTATUS），业务失败要向上抛。
    private void SendAck(uint type, object? payload)
    {
        IntPtr buf = IntPtr.Zero;
        try
        {
            buf = IoRoundtrip(type, payload);
            int status = Marshal.ReadInt32(buf, 8);          // FIX[P14]
            if (status != 0)
                throw new InvalidOperationException(
                    $"driver rejected msg {type}: NTSTATUS=0x{status:X8}");
        }
        finally { if (buf != IntPtr.Zero) Marshal.FreeHGlobal(buf); }
    }

    // 合并 send+recv: 按 T 解析 buf+16
    private T? Roundtrip<T>(uint type, object? payload) where T : struct
    {
        IntPtr buf = IntPtr.Zero;
        try
        {
            buf = IoRoundtrip(type, payload);
            int off = RbProtoDefs.RbMsgDataOffset;

            if (typeof(T) == typeof(RbStats))
            {
                var s = Marshal.PtrToStructure<RbStats>(buf + off);
                RbMarshal.NormalizeStats(ref s);
                return (T?)(object)s;
            }
            if (typeof(T) == typeof(RbConfig))
            {
                var c = Marshal.PtrToStructure<RbConfig>(buf + off);
                RbMarshal.NormalizeConfig(ref c);
                return (T?)(object)c;
            }
            if (typeof(T) == typeof(RbVolumeList))
            {
                var v = Marshal.PtrToStructure<RbVolumeList>(buf + off);
                RbMarshal.NormalizeVolumeList(ref v);
                return (T?)(object)v;
            }
            if (typeof(T) == typeof(RbLogList))
            {
                var l = Marshal.PtrToStructure<RbLogList>(buf + off);
                RbMarshal.NormalizeLogList(ref l);
                return (T?)(object)l;
            }
            if (typeof(T) == typeof(RbVersion))
            {
                var v = Marshal.PtrToStructure<RbVersion>(buf + off);
                RbMarshal.NormalizeVersion(ref v);
                return (T?)(object)v;
            }
            return null;
        }
        finally
        {
            if (buf != IntPtr.Zero) Marshal.FreeHGlobal(buf);
        }
    }

    public void Dispose()
    {
        if (_hDevice != NativeMethods.INVALID_HANDLE_VALUE && _hDevice != IntPtr.Zero)
        {
            NativeMethods.CloseHandle(_hDevice);
            _hDevice = NativeMethods.INVALID_HANDLE_VALUE;
        }
    }
}
