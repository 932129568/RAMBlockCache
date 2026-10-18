using System.Windows;

namespace RAMBlockCache.Controller;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // [FIX 严重-4 2026-10-18] 原 handler 无条件 Handled=true——UI 损坏态的静默放大器：
        // 磁盘页 2194 条/秒渲染异常被全吞（页面纯黑但"看起来正常"）、ProductVersion 绑定
        // 异常吞成"窗口不显示但进程活着"。改为 fail-fast：记日志后不吞，让 WPF 崩溃对话框
        // 出现（用户能感知并报告，而非拿着损坏的界面继续用）。
        DispatcherUnhandledException += (_, ex) =>
        {
            Services.LogService.Error($"未处理异常（fail-fast，不吞）：{ex.Exception.GetType().Name}：{ex.Exception.Message}\r\n{ex.Exception.StackTrace}");
            ex.Handled = false;   // 快速失败——宁可崩溃报告，不可静默半死界面
        };
        // [动效降级] 02b §B.5：WPF 无 CSS 媒体查询——启动期一次性求值：
        // 设置覆盖=关 → 降级；=开 → 强制开；=auto → 跟随系统动画开关
        // （SystemParameters.ClientAreaAnimation，用户在「调整 Windows 的外观和性能」里关动画即降级）。
        var motion = Services.SettingsService.Load().ReduceMotion;
        Controls.Motion.IsReducedMotion =
            motion == "off" || (motion != "on" && !SystemParameters.ClientAreaAnimation);
        base.OnStartup(e);
    }
}
