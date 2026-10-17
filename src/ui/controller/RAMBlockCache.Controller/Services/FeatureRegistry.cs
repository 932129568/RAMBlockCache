using System;
using System.Collections.Generic;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 预留特性服务注册表（W1–W4 占位功能的统一解析点）。
/// 设计约束（grill-me 2026-10-17 决策）：零新依赖、贴合项目单例风格——
/// 静态构造预注册各接口默认实现；运行期可 Register 替换（测试/演示/后续波次真实实现）。
/// </summary>
public static class FeatureRegistry
{
    private static readonly Dictionary<Type, object> _impls = new();
    private static readonly object _gate = new();

    /// <summary>预注册默认实现（进程生命周期内一次）。</summary>
    static FeatureRegistry()
    {
        Register<ITelemetryClient>(NoopTelemetryClient.Instance);
        Register<IFirstRunExperience>(NoopFirstRunExperience.Instance);
        Register<ILicenseService>(NoopLicenseService.Instance);
        Register<IDriveDisplayPolicy>(DefaultDriveDisplayPolicy.Instance);
        Register<IPrefetchGate>(DefaultPrefetchGate.Instance);
        Register<ISystemVolumeOptIn>(NoopSystemVolumeOptIn.Instance);
        Register<IL2RecoveryReporter>(NoopL2RecoveryReporter.Instance);
        Register<IErrorMessageProvider>(CatalogErrorMessageProvider.Instance);
        Register<IOperationProgress>(InMemoryOperationProgress.Instance);
    }

    /// <summary>注册/替换实现。返回 false=已存在同类型实现且未 force。</summary>
    public static bool Register<T>(T impl, bool force = false) where T : class
    {
        if (impl is null) throw new ArgumentNullException(nameof(impl));
        lock (_gate)
        {
            var t = typeof(T);
            if (!force && _impls.ContainsKey(t)) return false;
            _impls[t] = impl;
            return true;
        }
    }

    /// <summary>取实现（保证非 null；未注册=默认实现集不完整，属程序缺陷）。</summary>
    public static T Get<T>() where T : class
    {
        lock (_gate)
        {
            return _impls.TryGetValue(typeof(T), out var v)
                ? (T)v
                : throw new InvalidOperationException(
                    $"FeatureRegistry 未注册 {typeof(T).Name}（默认实现集不完整，属程序缺陷）");
        }
    }
}
