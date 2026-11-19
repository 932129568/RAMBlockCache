using System;
using System.Collections.Generic;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 预留特性服务注册表（W1–W4 占位功能的统一解析点）。
/// 设计约束（grill-me 2026-10-17 决策）：零新依赖、贴合项目单例风格——
/// 静态构造预注册各接口默认实现；运行期可 Register 替换（测试/演示/后续波次真实实现）。
/// [FIX U-03] 纪律修正：注册表 = 真实接线。后续波次接口**随消费方一起加入**，
/// 不再"先注册占位制造接线完备的错觉"（实测零消费方的 5 个接口已删）。
/// </summary>
public static class FeatureRegistry
{
    private static readonly Dictionary<Type, object> _impls = new();
    private static readonly object _gate = new();

    /// <summary>预注册默认实现（进程生命周期内一次）。</summary>
    static FeatureRegistry()
    {
        Register<ITelemetryClient>(NoopTelemetryClient.Instance);
        // [FIX U0-3] 首跑向导真实实现落地（W1-4）：Noop 占位（恒非首次运行 +
        // Reset 抛 NotImplemented）删除——注册表 = 真实接线。
        Register<IFirstRunExperience>(FirstRunExperience.Instance);
        Register<ILicenseService>(NoopLicenseService.Instance);
        Register<IErrorMessageProvider>(CatalogErrorMessageProvider.Instance);
        // [V10.6 契约接口层] B-4/B-5 二遍读执行体 / B-7+B-8 进度事件基座 / B-11 预取门：
        // 未就绪期默认实现（后端交付即 force 替换，消费方不变）。
        // IPresetService（B-3）由消费方 FeaturesViewModel 构造期携带适配器注册（依赖 VM 状态）。
        // [B-4 接线 2026-10-24] 后端 CLI bench/flushfile 已交付：exe 可定位（部署态同目录/
        // 开发态源码树）即注册真实执行体（测量纪律=CLI 同款路径）；定位失败回退 Deferred
        // （IsReady=false 演示数据+降级横幅，不冒充真结果）。
        var cli = CliBenchmarkService.TryCreate();
        Register<IBenchmarkService>(cli is not null ? cli : DeferredBenchmarkService.Instance);
        Register<IOperationProgress>(InProcessOperationProgress.Instance);
        Register<IPrefetchGate>(DefaultPrefetchGate.Instance);
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
