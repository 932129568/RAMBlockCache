using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace RamBoostFS.Wpf.ViewModels;

/// <summary>MVVM 基类：INotifyPropertyChanged 实现（wpf 树所有 VM/数据项的基类）。</summary>
public abstract class ObservableObject : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    /// <summary>置字段并触发变更通知；值相等则不动、返回 false。</summary>
    protected bool SetField<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        return true;
    }

    /// <summary>手动触发派生属性（计算列）的变更通知。</summary>
    protected void OnPropertyChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
