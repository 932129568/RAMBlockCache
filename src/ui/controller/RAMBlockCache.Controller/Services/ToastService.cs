using System;
using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Threading;
using RAMBlockCache.Controller.Helpers;

namespace RAMBlockCache.Controller.Services;

/// <summary>Toast 类型（语义色对齐 02b §A.2 语义系五色）。</summary>
public enum ToastKind { Info, Success, Warning, Danger }

/// <summary>Toast 数据项（IsClosing 驱动出场动画 DataTrigger）。</summary>
public sealed class ToastItem : ObservableObject
{
    public string Message { get; }
    public ToastKind Kind { get; }

    /// <summary>进入出场动画（模板 DataTrigger 监听；120ms ease-in 后移除）。</summary>
    public bool IsClosing { get; private set; }

    public ToastItem(string message, ToastKind kind)
    {
        Message = message;
        Kind = kind;
    }

    public void MarkClosing()
    {
        IsClosing = true;
        RaisePropertyChanged(nameof(IsClosing));
    }
}

/// <summary>
/// Toast 服务（02b §B.3-9：进 180ms ease-out / 出 120ms ease-in，位移 8px；3.5s 自动消退）。
/// 命令结果反馈面（W1-2 信任显性化的轻量形态）；同屏最多 3 条，超出挤掉最旧。
/// </summary>
public sealed class ToastService
{
    /// <summary>显示时长（ms，消退起点）。</summary>
    private const int ShowMs = 3500;
    /// <summary>出场动画时长（ms，与 02b §B.2 Toast 出场一致）。</summary>
    private const int ExitMs = 140;

    public static ToastService Instance { get; } = new();

    /// <summary>UI 绑定源（MainWindow 叠加层 ItemsControl）。</summary>
    public ObservableCollection<ToastItem> Toasts { get; } = new();

    /// <summary>弹出一条 Toast（任意线程调用，自动封送 UI 线程）。</summary>
    public void Show(string message, ToastKind kind = ToastKind.Info)
    {
        if (string.IsNullOrWhiteSpace(message)) return;
        var item = new ToastItem(message, kind);
        Run(() =>
        {
            Toasts.Add(item);
            while (Toasts.Count > 3) Toasts.RemoveAt(0);
            DismissLater(item);
        });
    }

    private static async void DismissLater(ToastItem item)
    {
        try
        {
            await System.Threading.Tasks.Task.Delay(ShowMs);
            Run(() =>
            {
                if (!item.IsClosing) item.MarkClosing();
                RemoveLater(item);
            });
        }
        catch (InvalidOperationException) { /* 应用关闭期 Dispatcher 已停，忽略 */ }
    }

    private static async void RemoveLater(ToastItem item)
    {
        try
        {
            await System.Threading.Tasks.Task.Delay(ExitMs);
            Run(() => Instance.Toasts.Remove(item));
        }
        catch (InvalidOperationException) { }
    }

    private static void Run(Action a)
    {
        var d = Application.Current?.Dispatcher;
        if (d is null || d.CheckAccess()) a();
        else d.BeginInvoke(a, DispatcherPriority.Normal);
    }
}
