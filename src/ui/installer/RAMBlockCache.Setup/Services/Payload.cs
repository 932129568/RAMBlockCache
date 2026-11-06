using System;
using System.IO;
using System.Reflection;

namespace RAMBlockCache.Setup.Services;

/// <summary>
/// 嵌入式载荷（U0-2 单 exe 化的核心）：安装器发布时把驱动/静默引擎/WPF 控制台/
/// CLI/INF/README 全部打进单一 exe 的资源区，安装期按需提取。
/// 命名约定：csproj 将 Assets/payload/** 以 LogicalName=payload.&lt;文件名&gt; 嵌入，
/// 因此本类只认扁平文件名，不感知目录。
/// </summary>
public static class Payload
{
    /// <summary>内核侧安装所需（提取到临时 staging，由静默引擎消费）。
    /// [P1.5 2026-11-06] boot_guard.ps1 / logon_disarm.ps1 随包（来源
    /// src\scripts\，由 build_setup.ps1 拷入 payload）：引擎安装时从自身所在
    /// staging 目录复制到 C:\ProgramData\RAMBoost 并武装双计划任务（看门协议）。
    /// 脚本只落 ProgramData，不进安装目录，故仅在 KernelFiles（staging 提取集）。 </summary>
    public static readonly string[] KernelFiles =
    {
        "ramboostsetup.exe", "RAMBlockCache.sys", "RAMBoostFS-Root.cer",
        "boot_guard.ps1", "logon_disarm.ps1",
    };

    /// <summary>安装目录落地文件（用户可见：控制台/CLI/INF/README）。
    /// [PACK 2026-11-04] 控制器改为 self-contained + webconsole 便携 node——
    /// 两者均为目录形态，以 zip 嵌入（Payload 仅支持扁平名），SetupEngine 解压。
    /// [FIX 2026-11-06] offline_rollback.cmd 随包：0x7B 一键离线回滚（WinRE 下
    /// 运行），此前只在 README 引用 src\scripts 路径——目标机不存在该路径。</summary>
    public static readonly string[] AppFiles =
    {
        "controller.zip", "webconsole.zip", "start-webconsole.cmd",
        "ramboostctl.exe", "rbfeatctl.exe",
        "RAMBlockCache.inf", "README.txt", "offline_rollback.cmd",
    };

    private static readonly Assembly Asm = typeof(Payload).Assembly;
    private static readonly HashSet<string> Names = new(Asm.GetManifestResourceNames(), StringComparer.OrdinalIgnoreCase);

    /// <summary>载荷是否已嵌入（未嵌入=开发期直跑，Assets/payload 为空）。</summary>
    public static bool Embedded => Names.Contains("payload.RAMBlockCache.sys");

    /// <summary>全部载荷文件名（Kernel + App，按序）。</summary>
    public static IEnumerable<string> AllFiles => KernelFiles.Concat(AppFiles);

    /// <summary>指定扁平名是否存在。</summary>
    public static bool Exists(string flatName) => Names.Contains($"payload.{flatName}");

    /// <summary>读取指定扁平名的字节；不存在抛清晰异常（fail-closed 不放行partial install）。</summary>
    public static byte[] Read(string flatName)
    {
        var rn = $"payload.{flatName}";
        using var s = Asm.GetManifestResourceStream(rn)
            ?? throw new FileNotFoundException($"嵌入式载荷缺失：{flatName}（资源名 {rn}）——请用 build_setup.ps1 构建发布包");
        using var ms = new MemoryStream();
        s.CopyTo(ms);
        return ms.ToArray();
    }

    /// <summary>提取单个文件到目标路径（覆盖写）。</summary>
    public static void ExtractTo(string flatName, string destPath)
    {
        var dir = Path.GetDirectoryName(destPath);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllBytes(destPath, Read(flatName));
    }

    /// <summary>批量提取到目录；progress 回报每个文件名与大小。</summary>
    public static void ExtractAllTo(IEnumerable<string> flatNames, string dir, IProgress<string>? progress = null)
    {
        Directory.CreateDirectory(dir);
        foreach (var f in flatNames)
        {
            ExtractTo(f, Path.Combine(dir, f));
            var len = new FileInfo(Path.Combine(dir, f)).Length;
            progress?.Report($"      提取 {f}（{len / 1024} KB）");
        }
    }

    /// <summary>载荷总字节数（UI 空间预估用）。</summary>
    public static long TotalBytes()
    {
        long t = 0;
        foreach (var f in AllFiles)
        {
            if (!Exists(f)) continue;
            using var s = Asm.GetManifestResourceStream($"payload.{f}");
            if (s is null) continue;
            t += s.Length;
        }
        return t;
    }
}
