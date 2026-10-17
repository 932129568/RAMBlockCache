using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Input;

namespace RAMBlockCache.Controller.Helpers;

/// <summary>
/// MVVM 基类：属性变更通知 + UI 线程投递。
/// </summary>
public abstract class ObservableObject : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    /// <summary>ViewModel 构造时捕获的 UI 线程同步上下文（构造线程即 UI 线程时非空）。</summary>
    private readonly SynchronizationContext? _ui = SynchronizationContext.Current;

    protected void RaisePropertyChanged([CallerMemberName] string? n = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
    protected bool Set<T>(ref T field, T value, [CallerMemberName] string? n = null)
    {
        if (Equals(field, value)) return false;
        field = value; RaisePropertyChanged(n); return true;
    }

    /// <summary>
    /// 把回调投递到 UI 线程执行；捕获不到同步上下文时直接执行。
    /// （原为 MainViewModel / DriveItemViewModel 各自持有的相同私有实现，重构时合并至此。）
    /// </summary>
    protected void Post(Action a) { if (_ui != null) _ui.Post(_ => a(), null); else a(); }
}

/// <summary>同步命令：包装 Action，可选 canExecute 谓词。</summary>
public sealed class RelayCommand : ICommand
{
    private readonly Action _exec;
    private readonly Func<bool>? _can;
    public RelayCommand(Action exec, Func<bool>? can = null) { _exec = exec; _can = can; }
    public bool CanExecute(object? p) => _can?.Invoke() ?? true;
    public void Execute(object? p) => _exec();
    public event EventHandler? CanExecuteChanged
    { add => CommandManager.RequerySuggested += value; remove => CommandManager.RequerySuggested -= value; }
}

/// <summary>
/// 异步命令：包装 Func&lt;Task&gt;，执行期间禁用重入（_busy）；
/// 未处理异常写 LogService，不冒泡到 UI 调度器。
/// </summary>
public sealed class AsyncRelayCommand : ICommand
{
    private readonly Func<Task> _exec;
    private readonly Func<bool>? _can;
    private bool _busy;
    public AsyncRelayCommand(Func<Task> exec, Func<bool>? can = null) { _exec = exec; _can = can; }
    public bool CanExecute(object? p) => !_busy && (_can?.Invoke() ?? true);
    public async void Execute(object? p)
    {
        if (!CanExecute(p)) return;
        _busy = true; CommandManager.InvalidateRequerySuggested();
        try { await _exec(); }
        catch (Exception ex) { Services.LogService.Error($"命令执行异常：{ex.Message}"); }
        finally { _busy = false; CommandManager.InvalidateRequerySuggested(); }
    }
    public event EventHandler? CanExecuteChanged
    { add => CommandManager.RequerySuggested += value; remove => CommandManager.RequerySuggested -= value; }
}
