using System;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using RAMBlockCache.Controller.Helpers;

namespace RAMBlockCache.Controller.Services;

/// <summary>Toast 严重度（决定语义色；颜色之外还有文案，不单靠颜色传意）。</summary>
public enum ToastLevel { Info, Success, Warning, Danger }

/// <summary>单条 Toast 视图项（进出场动画由宿主容器绑定 <see cref="Closing"/> 播放）。</summary>
public sealed class ToastItem : ObservableObject
{
    public string Text { get; }
    public ToastLevel Level { get; }
    public ToastItem(string text, ToastLevel level) { Text = text; Level = level; }

    private bool _closing;
    /// <summary>出场中（宿主据此项播出场动画后移除）。</summary>
    public bool Closing { get => _closing; set => Set(ref _closing, value); }
}

/// <summary>
/// [D5/W1-2] Toast 命令反馈面（02b §B.2/B.3-9：进 180ms ease-out / 出 120ms ease-in，位移 8px）。
/// 纪律：每条 Toast 必须来自真实命令完成事件（禁占位假接线）；UI 线程入队；
/// reduced-motion 降级时位移归零、仍完整显示（信息不丢）。
/// </summary>
public sealed class ToastService
{
    public static readonly ToastService Instance = new();

    /// <summary>当前 Toast 队列（宿主 ItemsControl 绑定；最多 3 条，超出挤最早）。</summary>
    public ObservableCollection<ToastItem> Toasts { get; } = new();

    private ToastService() { }

    /// <summary>入队一条 Toast（任意线程可调；内部 Post 到 UI）。</summary>
    public void Show(string text, ToastLevel level = ToastLevel.Info)
    {
        var app = Application.Current;
        if (app?.Dispatcher is not Dispatcher d) return;
        if (!d.CheckAccess()) { d.BeginInvoke(new Action(() => Show(text, level))); return; }
        Toasts.Add(new ToastItem(text, level));
        /* [FIX 2026-11-13] 旧实现对「挤压最旧项」的处理自相矛盾：先把 item0 标
         * Closing 并起 120ms 出场，紧接着 while 条件（Count>3）仍成立，于是下一
         * 轮看到 Toasts[0].Closing==true 走 else 分支 **立即 RemoveAt(0)** ——
         * 刚启动的出场动画在同一个调用栈里被摘掉，B22 的目标（挤压也走出场）
         * 实际从未发生，且随后 120ms 的 Task 还会去删一个已不存在的项。
         *
         * 现改为软/硬两级：软上限 3 条播 120ms 出场（期间允许短暂 4 条），
         * 只有突发堆积超过 4 条才硬摘，避免 Closing 项无限累积。 */
        while (Toasts.Count > HardLimit)
        {
            Toasts.RemoveAt(0);
        }
        if (Toasts.Count > SoftLimit)
        {
            var evicted = Toasts[0];
            if (!evicted.Closing)
            {
                evicted.Closing = true;
                _ = RemoveAfterExitAsync(evicted);
            }
        }
        var item = Toasts[^1];
        _ = DismissLaterAsync(item);   /* 4s 后自动出场；异常内部兜底不外溢 */
    }

    /// <summary>软上限：3 条同时可见。</summary>
    private const int SoftLimit = 3;

    /// <summary>硬上限：突发时允许短暂 4 条，超过则直接摘除最旧项。</summary>
    private const int HardLimit = 4;

    /// <summary>出场动画结束后摘除（不在 Show 的同一调用栈里立即摘）。</summary>
    private async Task RemoveAfterExitAsync(ToastItem item)
    {
        try
        {
            await Task.Delay(Controls.Motion.Dur120.TimeSpan).ConfigureAwait(true);
            var app = Application.Current;
            if (app?.Dispatcher is Dispatcher d && !d.HasShutdownStarted)
                d.Invoke(() => Toasts.Remove(item));
        }
        catch { /* Toast 装饰路径：任何失败静默 */ }
    }

    /// <summary>
    /// [FIX 2026-11-13] 由 <c>async void</c> 改为 <c>async Task</c>：async void 的
    /// 异常无法被调用方观察，只能依赖内部全捕获。改为 Task 后调用点显式丢弃，
    /// 语义仍是即发即忘，但不再依赖 async void 的隐式上下文行为。
    /// </summary>
    private async Task DismissLaterAsync(ToastItem item)
    {
        // [FIX QA5-UI8] 整段异常保护：关机/注销时 Dispatcher 已停摆，第二个 await
        // 之后的 Invoke 抛出会沿 async void 逃逸到 fail-fast 全局处理器 → 退出期
        // 崩溃对话框。异常一律静默吞（Toast 只是装饰，绝不能拖垮退出流程）。
        try
        {
            try { await Task.Delay(4000).ConfigureAwait(true); }
            catch { return; }
            if (!item.Closing) item.Closing = true;   // 宿主播出场动画
            await Task.Delay(Controls.Motion.Dur120.TimeSpan).ConfigureAwait(true);
            var app = Application.Current;
            if (app?.Dispatcher is Dispatcher d && !d.HasShutdownStarted)
                d.Invoke(() => Toasts.Remove(item));
        }
        catch { /* Toast 装饰路径：任何失败静默 */ }
    }

    /// <summary>语义色键（宿主 DataTrigger 用；颜色之外文案承载语义）。</summary>
    public static string BrushKey(ToastLevel level) => level switch
    {
        ToastLevel.Success => "RbcSuccessFg",
        ToastLevel.Warning => "RbcWarningFg",
        ToastLevel.Danger => "RbcDangerFg",
        _ => "RbcInfoFg"
    };

    /// <summary>背景色键（语义低透明底）。</summary>
    public static string BgKey(ToastLevel level) => level switch
    {
        ToastLevel.Success => "RbcSuccessBg",
        ToastLevel.Warning => "RbcWarningBg",
        ToastLevel.Danger => "RbcDangerBg",
        _ => "RbcInfoBg"
    };
}
