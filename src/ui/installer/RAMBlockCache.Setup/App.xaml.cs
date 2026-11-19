using System.Windows;

namespace RAMBlockCache.Setup;

/// <summary>
/// 安装器应用入口（U0-2）。职责：fail-fast 全局异常 handler（与 Controller 同款
/// 纪律——不吞异常，宁可崩溃报告也不要静默半死界面）+ 动效降级初值求值。
/// UI 层：安装器。
/// </summary>
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // [U0-2] fail-fast：未处理异常记日志后不置 Handled——安装器是 fail-closed
        // 关键路径上的门面，静默吞异常会让"失败零副作用"纪律失去观测面。
        DispatcherUnhandledException += (_, ex) =>
        {
            Services.SetupLog.Error($"未处理异常（fail-fast）：{ex.Exception.GetType().Name}：{ex.Exception.Message}\r\n{ex.Exception.StackTrace}");
            ex.Handled = false;
        };
        // [动效降级] 02b §B.5：安装器无设置页，仅跟随系统动画开关求初值。
        Controls.Motion.IsReducedMotion = !SystemParameters.ClientAreaAnimation;
        base.OnStartup(e);
    }
}
