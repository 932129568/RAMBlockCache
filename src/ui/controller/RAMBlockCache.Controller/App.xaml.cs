using System.Windows;

namespace RAMBlockCache.Controller;

public partial class App : Application
{
    /* [FIX S7-19] 多实例防护：原无全局唯一性检查——双开控制台时两个进程各自轮询
     * IOCTL、各自写 settings.json（后写覆盖先写）、托盘双图标，用户无法分辨哪个
     * 是"真的"。用 Local\ 命名互斥体（按用户会话隔离，多会话 RDP 各自可开）。 */
    private System.Threading.Mutex? _single;

    protected override void OnStartup(StartupEventArgs e)
    {
        _single = new System.Threading.Mutex(true, @"Local\RAMBlockCache.Controller.Single", out bool isNew);
        if (!isNew)
        {
            MessageBox.Show("RAMBlockCache 控制台已在运行（多实例会导致配置互相覆盖）。\n请在托盘找到已有实例。",
                "RAMBlockCache", MessageBoxButton.OK, MessageBoxImage.Information);
            Shutdown();
            return;
        }
        // [FIX 严重-4 2026-10-18] 原 handler 无条件 Handled=true——UI 损坏态的静默放大器：
        // 磁盘页 2194 条/秒渲染异常被全吞（页面纯黑但"看起来正常"）、ProductVersion 绑定
        // 异常吞成"窗口不显示但进程活着"。改为 fail-fast：记日志后不吞，让 WPF 崩溃对话框
        // 出现（用户能感知并报告，而非拿着损坏的界面继续用）。
        DispatcherUnhandledException += (_, ex) =>
        {
            Services.LogService.Error($"未处理异常（fail-fast，不吞）：{ex.Exception.GetType().Name}：{ex.Exception.Message}\r\n{ex.Exception.StackTrace}");
            ex.Handled = false;   // 快速失败——宁可崩溃报告，不可静默半死界面
        };
        // [动效降级] 02b §B.5：WPF 无 CSS 媒体查询——此处只按当前设置求初值：
        // 设置覆盖=关 → 降级；=开 → 强制开；=auto → 跟随系统动画开关
        // （SystemParameters.ClientAreaAnimation，用户在「调整 Windows 的外观和性能」里关动画即降级）。
        // [FIX U-04] 注释修正：并非"启动期一次性求值"——设置页 ReduceMotionIndex
        // setter 实时改判 Motion.IsReducedMotion（MainViewModel.cs:778-779）。
        var motion = Services.SettingsService.Load().ReduceMotion;
        Controls.Motion.IsReducedMotion =
            motion == "off" || (motion != "on" && !SystemParameters.ClientAreaAnimation);
        base.OnStartup(e);
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _single?.ReleaseMutex();
        _single?.Dispose();
        base.OnExit(e);
    }
}
