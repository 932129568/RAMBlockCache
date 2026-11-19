// 放置路径 1: src/ui/controller/RAMBlockCache.Controller/Interop/DwmLiquidGlassHelper.cs
// 放置路径 2: src/ui/installer/RAMBlockCache.Setup/Interop/DwmLiquidGlassHelper.cs
// 铁律: 落盘后必须同步登记至 build/sources.manifest.json (Gate16 check_build_manifest.py)
using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Shell;

namespace RAMBlockCache.Interop
{
    public static class DwmLiquidGlassHelper
    {
        private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
        private const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
        private const int DWMWA_BORDER_COLOR = 34;
        private const int DWMWA_SYSTEMBACKDROP_TYPE = 38;

        private const int DWMWCP_ROUND = 2;
        private const int DWMSBT_TRANSIENTWINDOW = 3; // DWM Transient Acrylic 桌面透光通道
        private const uint DWMWA_COLOR_NONE = 0xFFFFFFFE;

        [StructLayout(LayoutKind.Sequential)]
        private struct MARGINS
        {
            public int cxLeftWidth;
            public int cxRightWidth;
            public int cyTopHeight;
            public int cyBottomHeight;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ACCENT_POLICY
        {
            public int AccentState;
            public int AccentFlags;
            public uint GradientColor; // AABBGGRR
            public int AnimationId;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct WINDOWCOMPOSITIONATTRIBDATA
        {
            public int Attribute;
            public IntPtr Data;
            public int SizeOfData;
        }

        [DllImport("dwmapi.dll")]
        private static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref MARGINS pMarInset);

        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);

        [DllImport("dwmapi.dll")]
        private static extern int DwmGetColorizationColor(out uint pcrColorization, out bool pfOpaqueBlend);

        [DllImport("user32.dll")]
        private static extern int SetWindowCompositionAttribute(IntPtr hwnd, ref WINDOWCOMPOSITIONATTRIBDATA data);

        /// <summary>
        /// 启用硬件加速的桌面透光透色液态玻璃基底（不开启 AllowsTransparency，保留 DWM 帧率）
        /// 修 BUG-NEW-10：
        /// 1) 高对比度模式 (SystemParameters.HighContrast) 自动旁路保护；
        /// 2) Win11 22H2+ (Build >= 22621) 仅调用 DWMWA_SYSTEMBACKDROP_TYPE(38)，绝不叠加 WCA_ACCENT_POLICY(4)，
        ///    彻底避免 Win11 窗口鼠标拖拽掉帧与双层底色闪烁；仅在旧版系统降级使用 EnableAcrylicComposition。
        /// </summary>
        public static void AttachLiquidGlassWindow(Window window, bool darkMode = false)
        {
            if (window == null) return;

            if (SystemParameters.HighContrast)
            {
                window.Background = SystemColors.WindowBrush;
                return;
            }

            var chrome = new WindowChrome
            {
                CaptionHeight = 0,
                ResizeBorderThickness = new Thickness(6),
                GlassFrameThickness = new Thickness(-1),
                NonClientFrameEdges = NonClientFrameEdges.None,
                UseAeroCaptionButtons = false
            };
            WindowChrome.SetWindowChrome(window, chrome);
            window.Background = Brushes.Transparent;

            void OnSourceReady()
            {
                if (SystemParameters.HighContrast) return;

                IntPtr hwnd = new WindowInteropHelper(window).Handle;
                if (hwnd == IntPtr.Zero) return;

                var hwndSource = HwndSource.FromHwnd(hwnd);
                if (hwndSource?.CompositionTarget != null)
                {
                    hwndSource.CompositionTarget.BackgroundColor = Colors.Transparent;
                }

                var margins = new MARGINS { cxLeftWidth = -1, cxRightWidth = -1, cyTopHeight = -1, cyBottomHeight = -1 };
                DwmExtendFrameIntoClientArea(hwnd, ref margins);

                int darkFlag = darkMode ? 1 : 0;
                DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref darkFlag, sizeof(int));

                int corner = DWMWCP_ROUND;
                DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ref corner, sizeof(int));

                int borderNone = unchecked((int)DWMWA_COLOR_NONE);
                DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, ref borderNone, sizeof(int));

                bool backdropApplied = false;
                if (Environment.OSVersion.Version.Build >= 22621)
                {
                    int backdrop = DWMSBT_TRANSIENTWINDOW;
                    int hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int));
                    backdropApplied = (hr == 0);
                }

                if (!backdropApplied)
                {
                    byte alpha = darkMode ? (byte)0x38 : (byte)0x52;
                    uint bgr = darkMode ? 0x0020120Au : 0x00F9F5F3u;
                    EnableAcrylicComposition(hwnd, alpha, bgr);
                }
            }

            if (window.IsLoaded)
            {
                OnSourceReady();
            }
            else
            {
                // One-shot init: the handler below detaches itself as soon as the
                // HWND exists, so DWM setup can never run twice for this window.
                window.SourceInitialized += OnSourceInitialized;

                void OnSourceInitialized(object? sender, EventArgs e)
                {
                    window.SourceInitialized -= OnSourceInitialized;
                    OnSourceReady();
                }
            }
        }

        private static void EnableAcrylicComposition(IntPtr hwnd, byte alpha, uint bgr)
        {
            var accent = new ACCENT_POLICY
            {
                AccentState = 4, // ACCENT_ENABLE_ACRYLICBLURBEHIND (Win10 fallback only)
                AccentFlags = 2,
                GradientColor = ((uint)alpha << 24) | (bgr & 0x00FFFFFFu),
                AnimationId = 0
            };

            int size = Marshal.SizeOf(accent);
            IntPtr ptr = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(accent, ptr, false);
                var data = new WINDOWCOMPOSITIONATTRIBDATA
                {
                    Attribute = 19, // WCA_ACCENT_POLICY
                    Data = ptr,
                    SizeOfData = size
                };
                SetWindowCompositionAttribute(hwnd, ref data);
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
        }

        public static Color GetDesktopAccentColor()
        {
            if (DwmGetColorizationColor(out uint argb, out _) == 0)
            {
                return Color.FromArgb(
                    255,
                    (byte)((argb >> 16) & 0xFF),
                    (byte)((argb >> 8) & 0xFF),
                    (byte)(argb & 0xFF));
            }
            return Color.FromRgb(15, 134, 224); // 对齐 --rbc-accent: #0f86e0
        }

        /// <summary>
        /// 修 BUG-NEW-07：在当前交互用户会话 (Session >= 1) 将 DWM 色彩转换为 CSS HSL 令牌字符串，
        /// 供控制器内置 WebView2 通过 PostWebMessageAsJson 推送给前端 --rbc-lg-desktop-h1 / h2，
        /// 无需修改 Session 0 SYSTEM 计划任务运行的 Node Bridge。
        /// </summary>
        public static string BuildDesktopAccentMessageJson()
        {
            Color c = GetDesktopAccentColor();
            RgbToHsl(c.R, c.G, c.B, out int h, out int s, out int l);
            int h2 = (h + 346) % 360;

            // [284 2026-11-19] 补发 luma：前端按它把 scrim 在 0.10..0.38 之间插值。
            // 此前只发 h1/h2、从不发 luma，亮度自适应整条通路是空的，
            // 亮壁纸下 0.14 的白纱不够，白字压不住。口径 Rec.709 相对亮度。
            double luma = (0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B) / 255.0;
            luma = Math.Round(Math.Clamp(luma, 0.0, 1.0), 3);

            return string.Format(
                CultureInfo.InvariantCulture,
                "{{\"type\":\"rbc-desktop-accent\",\"h1\":\"{0} {1}% {2}%\",\"h2\":\"{3} {1}% {4}%\",\"luma\":{5}}}",
                h,
                Math.Clamp(s, 45, 92),
                Math.Clamp(l, 42, 62),
                h2,
                Math.Clamp(l - 4, 38, 58),
                luma);
        }

        private static void RgbToHsl(byte rByte, byte gByte, byte bByte, out int hDeg, out int sPct, out int lPct)
        {
            double r = rByte / 255.0;
            double g = gByte / 255.0;
            double b = bByte / 255.0;
            double max = Math.Max(r, Math.Max(g, b));
            double min = Math.Min(r, Math.Min(g, b));
            double d = max - min;
            double h = 0.0;
            double l = (max + min) / 2.0;
            double s = d == 0.0 ? 0.0 : d / (1.0 - Math.Abs(2.0 * l - 1.0));

            if (d != 0.0)
            {
                if (max == r) h = 60.0 * (((g - b) / d) % 6.0);
                else if (max == g) h = 60.0 * (((b - r) / d) + 2.0);
                else h = 60.0 * (((r - g) / d) + 4.0);
                if (h < 0.0) h += 360.0;
            }

            hDeg = (int)Math.Round(h);
            sPct = (int)Math.Round(s * 100.0);
            lPct = (int)Math.Round(l * 100.0);
        }
    }
}
