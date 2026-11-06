using System;
using System.Linq;
using System.Windows;
using RAMBlockCache.Setup.Services;
using RAMBlockCache.Setup.ViewModels;

namespace RAMBlockCache.Setup;

/// <summary>
/// 安装器主窗口（U0-2）。职责：按启动参数分流模式（/Uninstall=卸载，默认=安装
/// 四页向导）；装载 VM；页面切换入场动效（02b §B.2）。
/// UI 层：安装器。
/// </summary>
public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        bool uninstall = Environment.GetCommandLineArgs().Any(a =>
            string.Equals(a, "/Uninstall", StringComparison.OrdinalIgnoreCase));
        var vm = new InstallerViewModel(uninstall);
        DataContext = vm;
        Title = uninstall ? "RAMBlockCache 卸载" : "RAMBlockCache 安装";
        Loaded += (_, _) =>
        {
            SetupLog.Info(uninstall ? "uninstaller started" : "installer wizard started");
            Controls.Motion.PlayPageEnter((FrameworkElement)Content);
        };
        // [FIX QA5-UI6] 关窗语义可见化：Busy 时关窗不会中止引擎进程（引擎会自行
        // 完成，fail-closed 保证未做不可逆变更）——必须明示，不能静默放走。
        Closing += (s, e) =>
        {
            if (vm is { Busy: true } && !vm.ForceClose)
            {
                var r = MessageBox.Show("安装引擎仍在后台运行。关闭窗口不会中止它，\n" +
                                        "引擎会自行完成（未做不可逆变更）。\n\n确定关闭？",
                    Title, MessageBoxButton.YesNo, MessageBoxImage.Warning);
                if (r != MessageBoxResult.Yes) { e.Cancel = true; return; }
                vm.ForceClose = true;   // 用户确认：放行关闭
            }
        };
    }
}
