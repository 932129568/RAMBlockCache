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
using RAMBlockCache.Controller.Services;

namespace RAMBlockCache.Interop
{
    public static class DwmLiquidGlassHelper
    {
        private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
        private const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
        private const int DWMWA_BORDER_COLOR = 34;
        private const int DWMWA_SYSTEMBACKDROP_TYPE = 38;

        private const int DWMWCP_ROUND = 2;
        // DWMSBT 枚举（微软官方 dwm_systembackdrop_type）：
        //   0 AUTO / 1 NONE / 2 MAINWINDOW(Mica) / 3 TRANSIENTWINDOW(Desktop Acrylic) / 4 TABBEDWINDOW(Mica Alt)
        // [292] 已查证 Mica(2) 是 opaque 且只取壁纸色调、失焦退化纯色 ⇒ 不能透"窗口背后内容"，
        // 因此不作默认。默认保留 Desktop Acrylic(3)，它是唯一真透背后内容的系统材料。
        // [293] NONE(1) 加入可选项：用于验证 WebView2 在无 backdrop 载体时的真实行为，
        // 见下方 backdrop 选择处的完整记录（环境变量 RB_GLASS_MODE）。
        private const int DWMSBT_NONE = 1;
        private const int DWMSBT_MAINWINDOW = 2;      // Mica：opaque，仅作对照
        private const int DWMSBT_TRANSIENTWINDOW = 3; // Desktop Acrylic：透出窗口背后内容，浊度系统固定不可调
        private const int DWMSBT_TABBEDWINDOW = 4;    // Mica Alt：同样 opaque，仅作对照
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

                // [284 可观测性] 每个 DWM 调用的 HRESULT 必须落盘。
                // 为什么必须加：用户反馈"还是蓝白板"时，日志里只有"透色已生效"——
                // 那只证明 CSS 类挂上了，完全不能证明 DWM 亚克力通道打开了。
                // 这两件事是独立的：类挂上但亚克力没开 = 用户看到的仍是白底。
                // 只有 HRESULT 能区分"API 调用成功"与"API 被系统拒绝"。
                var hwndSource = HwndSource.FromHwnd(hwnd);
                if (hwndSource?.CompositionTarget != null)
                {
                    hwndSource.CompositionTarget.BackgroundColor = Colors.Transparent;
                }

                var margins = new MARGINS { cxLeftWidth = -1, cxRightWidth = -1, cyTopHeight = -1, cyBottomHeight = -1 };
                int hrExtend = DwmExtendFrameIntoClientArea(hwnd, ref margins);

                int darkFlag = darkMode ? 1 : 0;
                int hrDark = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref darkFlag, sizeof(int));

                int corner = DWMWCP_ROUND;
                int hrCorner = DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ref corner, sizeof(int));

                int borderNone = unchecked((int)DWMWA_COLOR_NONE);
                int hrBorder = DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, ref borderNone, sizeof(int));

                bool backdropApplied = false;
                int hrBackdrop = -1;
                int osBuild = Environment.OSVersion.Version.Build;
                int backdrop = ResolveBackdropType(out bool wantWcaAcrylic);
                string backdropMode = DescribeBackdropMode(backdrop, wantWcaAcrylic);

                if (osBuild >= 22621 && !wantWcaAcrylic)
                {
                    // [292 2026-11-19] 曾试过 DWMSBT_MAINWINDOW(2)=Mica 消除"磨砂感"，经查证后**撤回**：
                    //   微软文档明确写 Mica 是 "an opaque, dynamic material"，且 "it only samples
                    //   the desktop wallpaper once"。即 Mica ①不透明 ⇒ 窗口后面的其它窗口完全看不见；
                    //   ②只取壁纸色调 ⇒ 本机亮青壁纸被白色调洗后更贴近奶白板；
                    //   ③窗口失焦时退化为 SolidBackgroundFillColorBase 纯色。
                    //   换 Mica 是把"能透形"换成"更白且不能透形"，与诉求正好相反。
                    //
                    // [293 2026-11-19] 用户判读为"透得还不够，背景是糊的" ⇒ 瓶颈确认为系统 Acrylic
                    //   的重磨砂（浊度固定不可调）。此时必须先承认一条硬事实，否则会走错方向：
                    //   **CSS backdrop-filter 采样不到桌面像素**。Chromium 明确说明它只模糊
                    //   "浏览器渲染上下文内的内容"，窗口下方的内容由操作系统处理，页面无从访问
                    //   （nwjs#7399 维护者原文：it calculates the pixels to render based on what is
                    //   on the page, it does not have any context for what is under the window）。
                    //   推论：不存在"关掉 DWM 让页面自己模糊"的方案 —— 关掉只会得到**清晰透明**的
                    //   桌面（形全透出来、毫无模糊），而不是可控半径的模糊。模糊半径的唯一来源
                    //   就是 DWM 本身。因此可做的实验只有换 DWM 材质，不是改页面滤镜。
                    //
                    // 为了让"哪种材质最接近液态玻璃"由目视而不是由推理决定，材质改为可切换：
                    //   环境变量 RB_GLASS_MODE = auto(默认) | transient | none | mica | tabbed | acrylic
                    //   auto 行为与 292 完全一致（transient），不改变默认交付形态。
                    hrBackdrop = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int));
                    backdropApplied = (hrBackdrop == 0);
                }

                int hrAcrylic = 0;
                if (!backdropApplied)
                {
                    byte alpha = darkMode ? (byte)0x38 : (byte)0x52;
                    uint bgr = darkMode ? 0x0020120Au : 0x00F9F5F3u;
                    hrAcrylic = EnableAcrylicComposition(hwnd, alpha, bgr);
                }

                // 单行汇总，便于 grep 与粘贴比对：
                //   mode ⇒ 本次实际生效的材质（由 RB_GLASS_MODE 决定，默认 transient）
                //   extend/hr 非 0 ⇒ 玻璃帧扩展失败（整窗不透的根因）
                //   backdrop/hr 非 0 且 acrylic/hr 非 0 ⇒ 两条通道都失败
                LogService.Info(
                    $"[284/DWM] build={osBuild} mode={backdropMode} extend=0x{hrExtend:X8} dark=0x{hrDark:X8} " +
                    $"corner=0x{hrCorner:X8} border=0x{hrBorder:X8} " +
                    $"backdrop=0x{hrBackdrop:X8}({(backdropApplied ? "applied" : "rejected")}) " +
                    $"acrylic=0x{hrAcrylic:X8} " +
                    $"sourceBg={(hwndSource?.CompositionTarget?.BackgroundColor.ToString() ?? "null")}");
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

        private static int EnableAcrylicComposition(IntPtr hwnd, byte alpha, uint bgr)
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
                return SetWindowCompositionAttribute(hwnd, ref data);
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
        }

        /// <summary>
        /// [293] 把 RB_GLASS_MODE 映射为 DWM 材质常量。公开是为了让它可被测试真正调用 ——
        /// 读源码正则做声明断言抓不住"开关被忽略"这类缺陷（读到一个 case 就通过），
        /// 只有真的调一次、比较返回值，才能证明这个开关有牙。
        /// </summary>
        public static int ResolveBackdropType(out bool wantWcaAcrylic)
        {
            wantWcaAcrylic = false;
            string mode = (Environment.GetEnvironmentVariable("RB_GLASS_MODE") ?? string.Empty)
                .Trim()
                .ToLowerInvariant();

            switch (mode)
            {
                case "none":
                    return DWMSBT_NONE;
                case "mica":
                    return DWMSBT_MAINWINDOW;
                case "tabbed":
                    return DWMSBT_TABBEDWINDOW;
                case "acrylic":
                case "wca":
                    // 强制走旧的 WCA_ACCENT_POLICY 分支（Win10 降级通道），
                    // 用于对照"系统材料"与"强调色注入"两种模糊来源的观感差异。
                    wantWcaAcrylic = true;
                    return DWMSBT_TRANSIENTWINDOW;
                case "transient":
                case "":
                case "auto":
                default:
                    return DWMSBT_TRANSIENTWINDOW;
            }
        }

        private static string DescribeBackdropMode(int backdrop, bool wantWcaAcrylic)
        {
            if (wantWcaAcrylic) return "wca-acrylic";
            switch (backdrop)
            {
                case DWMSBT_NONE: return "none";
                case DWMSBT_MAINWINDOW: return "mica";
                case DWMSBT_TABBEDWINDOW: return "tabbed";
                case DWMSBT_TRANSIENTWINDOW: return "transient";
                default: return "unknown(" + backdrop + ")";
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
        ///
        /// [284 2026-11-19] 曾补发 luma（壁纸亮度 0..1），供前端在 0.10..0.38 之间
        /// 插值 scrim。该字段的**数据源是错的**，292 已改为固定 -1 哨兵，
        /// 具体理由见下方方法体内的 292 记录 —— 简述：强调色亮度冒充壁纸亮度，
        /// 把一个无关的浅色主题色变成了"整页白纱 0.34"的直接原因。
        /// </summary>
        public static string BuildDesktopAccentMessageJson()
        {
            Color c = GetDesktopAccentColor();
            RgbToHsl(c.R, c.G, c.B, out int h, out int s, out int l);
            int h2 = (h + 346) % 360;

            // [292 2026-11-19] 这里原来算的是 AccentPalette 首色的 Rec.709 亮度，
            // 然后当作"壁纸亮度"发给前端。这是本次"只能透色一点"的主因，不能保留：
            //   本机 AccentPalette 首色 = #99EBFF（浅青），luma = 0.859
            //   前端插值：0.10 + 0.859 × 0.28 = 0.340
            //   ⇒ 用户看到的白纱是 0.34，不是 tokens.css 里写的 0.14
            //   ⇒ 再叠 section 0.16 + card 0.26，合成 0.59，玻璃只剩 41% 透射。
            //
            // 两处语义错误（不是调参问题）：
            //   1) 数据源错：DwmGetColorizationColor 给的是**系统强调色**，不是壁纸亮度。
            //      用它冒充 luma，等于"主题色越浅玻璃越白"，与壁纸明暗毫无关系；
            //      而真正影响白字可读性的是窗口背后那块壁纸。
            //   2) 方向错：本应用正文是深色 #17233c（亮色主题），需要的是
            //      "暗壁纸多盖白纱、亮壁纸少盖"。旧注释写的"亮壁纸多盖"是为
            //      白字场景写的，与本应用正好相反。
            //
            // 处置：不再用强调色伪造 luma，固定发 -1 作为"宿主无此数据"的哨兵。
            // 前端只接受 [0,1] 的有限数，-1 必然被跳过 ⇒ 回落到 tokens.css 的唯一
            // 真相源。不用 0：0 会被当成"纯黑壁纸"，把 scrim 压到最低端，方向相反。
            // 真正的壁纸亮度要 SystemParametersInfo + 壁纸像素采样，属独立特性，
            // 不在本次缺陷修复范围内 —— 宁可不做，也不再用错误的值驱动视觉。
            const double luma = -1;

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
