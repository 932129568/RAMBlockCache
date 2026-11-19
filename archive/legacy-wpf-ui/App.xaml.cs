using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Threading;
using RamBoostFS.Wpf.Models;

namespace RamBoostFS.Wpf;

public partial class App
{
    /// <summary>
    /// P0-5 启动自检: 触碰 RbMsg 及各 payload 结构, 确保 CLR 能加载、StructureToPtr 不抛。
    /// 失败即写 selfcheck.log 并弹错 (绝不静默)。
    /// </summary>
    private static void MarshalSelfTest()
    {
        try
        {
            int off = RbProtoDefs.RbMsgDataOffset; // V8 = 16
            // 1) 触碰 RbMsg 类型 (以前会 TypeLoadException)
            var hdr = new RbMsg { Type = 1, Size = (uint)RbProtoDefs.RbMsgSize, Status = 0, Reserved = 0 };

            // 2) 每个 payload 各 marshal 一次
            IntPtr buf = Marshal.AllocHGlobal(RbProtoDefs.RbMsgSize);
            try
            {
                Marshal.WriteInt32(buf, 0, (int)hdr.Type);
                Marshal.WriteInt32(buf, 4, (int)hdr.Size);

                var cfg = new RbConfig(); RbMarshal.NormalizeConfig(ref cfg);
                Marshal.StructureToPtr(cfg, buf + off, false);

                var st = new RbStats(); RbMarshal.NormalizeStats(ref st);
                Marshal.StructureToPtr(st, buf + off, false);

                var vl = new RbVolumeList(); RbMarshal.NormalizeVolumeList(ref vl);
                Marshal.StructureToPtr(vl, buf + off, false);

                var ll = new RbLogList(); RbMarshal.NormalizeLogList(ref ll);
                Marshal.StructureToPtr(ll, buf + off, false);

                var ver = new RbVersion(); RbMarshal.NormalizeVersion(ref ver);
                Marshal.StructureToPtr(ver, buf + off, false);

                // 反向 PtrToStructure 一次
                Marshal.PtrToStructure<RbMsg>(buf);
            }
            finally { Marshal.FreeHGlobal(buf); }

            File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "selfcheck.log"),
                $"OK RbMsg loadable, RbMsgSize={RbProtoDefs.RbMsgSize}, dataOff={off}, IOCTL=0x{RbProtoDefs.IOCTL_RB_REQUEST:X} @ {DateTime.Now:o}");
        }
        catch (Exception ex)
        {
            File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "selfcheck.log"), "FAIL: " + ex);
            MessageBox.Show("RbMsg marshal 自检失败:\n" + ex, "selfcheck");
        }
    }

    protected override void OnStartup(StartupEventArgs e)
    {
        MarshalSelfTest();

        // [V10.1] 控制面崩溃隔离: 控制台崩了绝不影响内核缓存。
        const string crashLog = @"C:\ProgramData\RAMBoost\ui_crash.log";

        DispatcherUnhandledException += (s, args) =>
        {
            try
            {
                File.AppendAllText(crashLog,
                    $"[{DateTime.Now:HH:mm:ss}] {args.Exception}\r\n");
            }
            catch { }
            MessageBox.Show("界面发生错误已拦截（驱动不受影响），详情见 ui_crash.log",
                "RAMBoost", MessageBoxButton.OK, MessageBoxImage.Warning);
            args.Handled = true;   // 不崩进程, 自愈
        };
        AppDomain.CurrentDomain.UnhandledException += (s, args) =>
        {
            try
            {
                File.AppendAllText(crashLog,
                    $"[{DateTime.Now:HH:mm:ss}] FATAL {args.ExceptionObject}\r\n");
            }
            catch { }
        };
        base.OnStartup(e);
    }
}

