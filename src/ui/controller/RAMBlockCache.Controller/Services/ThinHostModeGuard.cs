using System;

namespace RAMBlockCache.Controller.Services
{
    /// <summary>
    /// 宿主薄壳模式守卫（240 号文档模块 5）。
    ///
    /// 事实核验（2026-11-14）：本仓库 MainWindow 已是纯 WebView2 壳，
    /// MainViewModel 全仓无实例化点，DriverEventPoller 不会随窗口启动，
    /// 因此文档 ARCH-05 描述的「C# 与 SPA 双端重复轮询」在当前代码里并不成立。
    /// 本类型按 R10（不扩大范围）仅作为设计层守卫落盘并纳入编译验证，
    /// 未接入任何轮询调用链；若后续重新启用原生 ViewModel，可用
    /// ShouldAllowLegacyCSharpPolling 在启动采样前做显式门控。
    /// </summary>
    public sealed class ThinHostModeGuard
    {
        private static readonly Lazy<ThinHostModeGuard> LazyInstance =
            new Lazy<ThinHostModeGuard>(() => new ThinHostModeGuard());

        public static ThinHostModeGuard Instance => LazyInstance.Value;

        private volatile bool _isWebViewUiReady;

        private ThinHostModeGuard() { }

        public bool IsWebViewUiReady => _isWebViewUiReady;

        /// <summary>WebView2 尚未就绪时，才允许 C# 宿主做最小化健康探测。</summary>
        public bool ShouldAllowLegacyCSharpPolling => !_isWebViewUiReady;

        public event EventHandler<bool>? HostModeChanged;

        public void MarkWebViewUiReady()
        {
            if (_isWebViewUiReady) return;
            _isWebViewUiReady = true;
            HostModeChanged?.Invoke(this, true);
        }

        public void MarkWebViewFallback()
        {
            if (!_isWebViewUiReady) return;
            _isWebViewUiReady = false;
            HostModeChanged?.Invoke(this, false);
        }
    }
}
