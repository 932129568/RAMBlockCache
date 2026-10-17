using System.Windows;

namespace RAMBlockCache.Controller;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        DispatcherUnhandledException += (_, ex) =>
        {
            Services.LogService.Error($"未处理异常：{ex.Exception.Message}");
            ex.Handled = true;
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
