using System.Windows;
using RAMBlockCache.Controller.Services;
using RAMBlockCache.Controller.ViewModels;

namespace RAMBlockCache.Controller;

public partial class MainWindow : Window
{
    /// <summary>构造：DataContext=MainViewModel；Loaded 后枚举卷，Closing 时停统计轮询。</summary>
    public MainWindow()
    {
        InitializeComponent();
        DataContext = new MainViewModel();
        // [状态条] VM 侧注入控件引用（UpdateStateBar 经此驱动渲染，VM 不直接摸可视树）
        if (DataContext is MainViewModel vm0) vm0.StateBarCtl = StateBarCtl;
        Loaded += (_, _) => (DataContext as MainViewModel)?.RefreshDrives();
        Closing += (_, _) => Services.StatsService.Instance.Stop();

        // [状态条] 段点击 → 路由到处置页（ViewModel 持有 SelectedPage）
        StateBarCtl.NavigateRequested += page =>
        {
            if (DataContext is MainViewModel vm && vm.SelectedPage != page)
                vm.SelectedPage = page;
        };

        // [动效 02b §B.2] 页面切换入场：淡入 + translateY 8px→0（240ms ease-out）。
        // 保留 DataTrigger+Visibility 结构不动，只外挂 IsVisibleChanged 触发器——
        // 首屏（总览）Loaded 后也会播一次入场。降级时 PlayPageEnter 直达终态。
        foreach (var page in new FrameworkElement[] { PageOverview, PageDisks, PageCache, PageLog, PageSettings })
        {
            page.IsVisibleChanged += (s, _) =>
            {
                if (s is FrameworkElement fe && fe.IsVisible)
                    Controls.Motion.PlayPageEnter(fe);
            };
        }
    }
}
