using System.Windows;
using RAMBlockCache.Controller.Services;
using RAMBlockCache.Controller.ViewModels;

namespace RAMBlockCache.Controller.Views;

/// <summary>
/// 首跑向导窗口（U0-3）。完成时经 IFirstRunExperience.MarkCompleted 持久化
/// FirstRunDone；动效入场复用 Controls/Motion（02b §B.2）。
/// UI 层：UI。
/// </summary>
public partial class FirstRunWizard : Window
{
    public FirstRunWizard()
    {
        InitializeComponent();
        DataContext = new FirstRunWizardViewModel();
        if (DataContext is FirstRunWizardViewModel vm) vm.Completed += CompleteAndClose;
        Loaded += (_, _) => Controls.Motion.PlayPageEnter((FrameworkElement)Content);
    }

    /// <summary>完成按钮（NextText=“完成”时由 VM 的 NextCommand 推进到此处调用）。</summary>
    public void CompleteAndClose()
    {
        FeatureRegistry.Get<IFirstRunExperience>().MarkCompleted();
        LogService.Info("首跑向导完成（FirstRunDone 已持久化）");
        Close();
    }
}
