using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace RAMBlockCache.Setup.Helpers;

/// <summary>
/// MVVM 基类：属性变更通知（与 Controller Helpers/RelayCommand.cs 同款，自包含）。
/// UI 层：安装器向导。
/// </summary>
public abstract class ObservableObject : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected void RaisePropertyChanged([CallerMemberName] string? n = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));

    protected bool Set<T>(ref T field, T value, [CallerMemberName] string? n = null)
    {
        if (Equals(field, value)) return false;
        field = value; RaisePropertyChanged(n); return true;
    }
}

/// <summary>同步命令：包装 Action，可选 canExecute 谓词。</summary>
public sealed class RelayCommand : System.Windows.Input.ICommand
{
    private readonly Action _exec;
    private readonly Func<bool>? _can;
    public RelayCommand(Action exec, Func<bool>? can = null) { _exec = exec; _can = can; }
    public bool CanExecute(object? p) => _can?.Invoke() ?? true;
    public void Execute(object? p) => _exec();
    public event EventHandler? CanExecuteChanged
    { add => System.Windows.Input.CommandManager.RequerySuggested += value; remove => System.Windows.Input.CommandManager.RequerySuggested -= value; }
}
