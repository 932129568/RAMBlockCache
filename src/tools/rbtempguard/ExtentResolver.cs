using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace RbTempGuard;

/// <summary>
/// 把文件在卷上的物理簇范围（LCN 区间）解析为卷内字节区间。
///
/// 为什么需要它：RAMBlockCache 是卷块层过滤驱动，看不到文件名。要在块层
/// "排除临时文件/软件缓存"，唯一可行的路径是：用户态用
/// FSCTL_GET_RETRIEVAL_POINTERS 拿到文件的数据区映射（VCN→LCN），
/// 换算成卷内字节偏移后下推给驱动（见 ctl/rb_excl.h）。
///
/// 注意点：
///  - 稀疏/压缩文件的 Extent 里 Lcn == -1 表示"无物理簇"，必须跳过；
///  - 常驻（resident）小文件没有数据区映射，调用会返回 0 个 extent；
///  - 文件在两次枚举之间被改写会导致映射过期——这是"缓存放置提示"，
///    过期最坏结果是多/少排除几个块，不会造成数据错误（见 cache.h 契约）。
/// </summary>
internal static class ExtentResolver
{
    private const uint FSCTL_GET_RETRIEVAL_POINTERS = 0x00090073; // CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 28, METHOD_NEITHER, ANY)
    private const int ErrorMoreData = 234;
    private const int ErrorHandleEof = 38;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes,
        uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(
        SafeFileHandle hDevice, uint dwIoControlCode,
        IntPtr lpInBuffer, int nInBufferSize,
        IntPtr lpOutBuffer, int nOutBufferSize,
        out int lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool GetDiskFreeSpaceW(
        string lpRootPathName, out uint lpSectorsPerCluster, out uint lpBytesPerSector,
        out uint lpNumberOfFreeClusters, out uint lpTotalNumberOfClusters);

    /// <summary>每簇字节数（同一卷内恒定）。</summary>
    public static long GetClusterSize(string root)
    {
        if (!GetDiskFreeSpaceW(root, out uint spc, out uint bps, out _, out _))
            return 4096; // 保守缺省：NTFS 常见值，最坏只是区间对齐偏差
        long v = (long)spc * bps;
        return v > 0 ? v : 4096;
    }

    /// <summary>
    /// 取单个文件的物理区间（卷内字节偏移）。失败返回 false 并通过 <paramref name="why"/> 说明。
    /// <paramref name="maxAdd"/> 限制本次最多追加多少区间（调用方预算控制，避免超大碎片文件把预算吃光）。
    /// </summary>
    public static bool TryGetFileExtents(string path, long clusterSize,
                                         List<(ulong Start, ulong Len)> sink, int maxAdd,
                                         out string why)
    {
        why = null;
        // 只读属性访问即可：FSCTL_GET_RETRIEVAL_POINTERS 不读文件数据
        using SafeFileHandle h = CreateFileW(path, 0x80000000u /*GENERIC_READ*/,
            0x7u /*SHARE_READ|WRITE|DELETE*/, IntPtr.Zero, 3 /*OPEN_EXISTING*/,
            0x02000000u /*FILE_FLAG_BACKUP_SEMANTICS*/, IntPtr.Zero);
        if (h.IsInvalid)
        {
            int e = Marshal.GetLastWin32Error();
            why = e switch
            {
                5 => "access denied",
                32 => "in use",
                2 => "not found",
                _ => "open 0x" + e.ToString("X")
            };
            return false;
        }

        long startVcn = 0;
        int guard = 0;
        int outSize = 16 * 1024;
        while (guard++ < 4096 && startVcn >= 0)
        {
            byte[] inBuf = BitConverter.GetBytes(startVcn);
            byte[] outBuf = new byte[outSize];
            GCHandle hIn = GCHandle.Alloc(inBuf, GCHandleType.Pinned);
            GCHandle hOut = GCHandle.Alloc(outBuf, GCHandleType.Pinned);
            bool ok;
            int ret = 0;
            try
            {
                ok = DeviceIoControl(h, FSCTL_GET_RETRIEVAL_POINTERS,
                    hIn.AddrOfPinnedObject(), inBuf.Length,
                    hOut.AddrOfPinnedObject(), outBuf.Length,
                    out ret, IntPtr.Zero);
            }
            finally
            {
                hIn.Free();
                hOut.Free();
            }

            int err = ok ? 0 : Marshal.GetLastWin32Error();
            if (!ok && err != ErrorMoreData && err != ErrorHandleEof)
            {
                // 常驻文件 / 卷已卸载 / 无权限：都不是致命错误，跳过该文件
                why = "fsctl 0x" + err.ToString("X");
                return false;
            }

            // RETRIEVAL_POINTERS_BUFFER: DWORD ExtentCount; LARGE_INTEGER StartingVcn;
            //   { LARGE_INTEGER NextVcn; LARGE_INTEGER Lcn; } Extents[];
            int count = BitConverter.ToInt32(outBuf, 0);
            int maxFit = (outBuf.Length - 16) / 16;
            if (count > maxFit) count = maxFit;
            for (int i = 0; i < count; i++)
            {
                if (sink.Count >= maxAdd) return true;      // 预算用尽：保留已解析部分即可
                int off = 16 + i * 16;
                long nextVcn = BitConverter.ToInt64(outBuf, off);
                long lcn = BitConverter.ToInt64(outBuf, off + 8);
                long prevVcn = (i == 0) ? startVcn : BitConverter.ToInt64(outBuf, off - 16);
                long clusters = nextVcn - prevVcn;
                if (clusters <= 0) continue;
                if (lcn < 0) continue;                     // 稀疏段：无物理簇
                sink.Add(((ulong)(lcn * clusterSize), (ulong)(clusters * clusterSize)));
            }

            if (!ok && err == ErrorMoreData && count > 0)
            {
                startVcn = BitConverter.ToInt64(outBuf, 16 + (count - 1) * 16); // 最后一条的 NextVcn
                if (outSize < 1024 * 1024) outSize *= 2;   // 碎片化严重时放大缓冲
                continue;
            }
            return true;
        }
        return true;
    }
}
