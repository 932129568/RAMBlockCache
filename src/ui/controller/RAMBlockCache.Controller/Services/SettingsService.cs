using System;
using System.IO;
using System.Text.Json;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 控制台本地设置（序列化为 %LOCALAPPDATA%\RAMBlockCache\settings.json）。
/// JSON 键名 = 属性名（System.Text.Json 默认），重构期键名只读。
/// </summary>
public sealed class AppSettings
{
    // [FIX U-02] 删除每卷配额/写模式两个占位字典：零读写的死数据
    // （用户选择重启即重置的真功能缺口显性化，去向随 W2-3 配置面产品化统一做）。
    // 存量 settings.json 的同名键被 System.Text.Json 默认忽略，无需迁移。

    /// <summary>开机自动随卷挂载（跳过系统盘 C:）。
    /// [FIX 0xA-2026-10-17] 默认关：打开控制台并发挂载 7 卷会轰炸 attach 路径
    /// （attach 收尾的 APC 交付窗口曾在本机炸出 0xA）。自动挂载须用户显式开启。</summary>
    public bool AutoStartWithVolumes { get; set; } = false;
    /// <summary>随 Windows 启动本控制台（写 Run 键，见 SystemService.SetLaunchOnBoot）。</summary>
    public bool LaunchOnBoot { get; set; }
    /// <summary>遥测采集 opt-in（W1-1 三原则：默认关、匿名、一键可关）。
    /// 占位期仅记录用户意愿，真实采集随 W1-1 波次落地（ITelemetryClient）。</summary>
    public bool TelemetryOptIn { get; set; }
    /// <summary>动效降级覆盖："auto"=跟随系统动画开关（默认）/ "on"=强制开 / "off"=强制关。
    /// 对应 02b §B.5 reduced-motion 的手动覆盖通道。</summary>
    public string ReduceMotion { get; set; } = "auto";
    /// <summary>首次向导已完成（W1-4；旧 settings.json 缺此键→false=按首次运行处理）。</summary>
    public bool FirstRunDone { get; set; }
    /// <summary>联调模式：界面上显示接口预留角标（A=已接线/B=待驱动字段/C=待接口/D=待实现），
    /// 用于前后端联调对齐「前端等什么后端」。默认开（2026-10-20 用户反馈无角标"像旧页面"，
    /// 接口预留位置需常显；需要干净产品视图时在设置页一键关）。</summary>
    public bool LinkMode { get; set; } = true;
    /// <summary>许可态（F5；B-14 服务端点就绪前的本地口径）：trial/active。</summary>
    public string LicenseState { get; set; } = "trial";
    /// <summary>[V10.6 F1] 向导内选择的场景预设档位（safe/balanced/perf；向导关闭后经 Features 应用）。</summary>
    public string WizardPreset { get; set; } = "balanced";
    /// <summary>试用起始日（yyyy-MM-dd；首跑自动落今天，30 天试用期双方冻结口径）。</summary>
    public string TrialStartDate { get; set; } = "";
    /// <summary>许可类型（已激活态显示，如"个人版"/"专业版"）。</summary>
    public string LicenseType { get; set; } = "";
}

/// <summary>settings.json 读写。读失败/损坏时回退默认设置，不抛。
/// [FIX 主-1 2026-10-19] 进程内单例：Load 每次返回新实例 + Save 整体序列化 = 多副本竞态——
/// MainViewModel 构造期快照（FirstRunDone=false）会在向导完成后被设置页任一开关
/// Save 回滚 → 向导每次启动重弹。现 Load 返回缓存实例，Save 写同一实例，
/// 向导（FirstRunExperience）与主 VM 天然共享同一份。</summary>
public static class SettingsService
{
    /* [FIX S7-13] 数据目录解析禁回落相对路径：GetFolderPath 在配置文件目录缺失/
     * 漫游异常时返回 ""，Path.Combine("", x) = 相对路径 → 设置/日志/诊断静默落到
     * 进程 CWD（装在 ProgramFiles 时只读 → 全部写失败；从可写目录启动则污染安装
     * 目录）。链路：GetFolderPath → LOCALAPPDATA 环境变量 → 用户profile 拼凑；
     * 都失败禁用持久化（Banner 明示），绝不使用相对路径。 */
    private static readonly string _dir = ResolveDataDir();

    private static string ResolveDataDir()
    {
        var p = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrEmpty(p)) p = Environment.GetEnvironmentVariable("LOCALAPPDATA");
        if (string.IsNullOrEmpty(p))
        {
            var root = Environment.GetEnvironmentVariable("USERPROFILE");
            if (!string.IsNullOrEmpty(root)) p = Path.Combine(root, "AppData", "Local");
        }
        if (string.IsNullOrEmpty(p)) return string.Empty;   /* 禁用持久化：Save/Load 走 catch */
        try { return Path.Combine(Path.GetFullPath(p), "RAMBlockCache"); }
        catch { return string.Empty; }
    }
    private static readonly string _file = Path.Combine(_dir, "settings.json");
    private static readonly JsonSerializerOptions _opt = new() { WriteIndented = true };
    private static AppSettings? _current;

    /// <summary>读取设置（进程内单例）；文件不存在或解析失败返回默认实例。</summary>
    public static AppSettings Load()
    {
        if (_current is not null) return _current;
        try { _current = JsonSerializer.Deserialize<AppSettings>(System.IO.File.ReadAllText(_file), _opt) ?? new(); }
        catch { _current = new(); }
        return _current;
    }

    /// <summary>写回 settings.json（自动创建目录）。
    /// [FIX 警告-5 2026-10-18] 原无异常保护：磁盘/权限失败直接抛进 setter，
    /// 被全局 handler 吞掉 → UI 已翻转但持久化静默失败。现失败记日志。</summary>
    public static void Save(AppSettings s)
    {
        _current = s;   /* [FIX 主-1] 单例锚点更新：后续 Load 一律返回最新实例 */
        try
        {
            if (string.IsNullOrEmpty(_dir))
            {
                LogService.Warn("settings.json 持久化已禁用（数据目录不可解析）；本次改动仅进程内生效");
                return;
            }
            Directory.CreateDirectory(_dir);
            /* [FIX S7-12] 原子写：原 WriteAllText 先截断再写，崩溃/掉电即留下
             * 空/半截 JSON → 下次 Load 回默认（设置全丢+向导重弹）。改为同目录
             * 临时文件 + File.Replace 原子换入（Replace 失败时退 Move 覆盖）。 */
            var tmp = _file + ".tmp";
            System.IO.File.WriteAllText(tmp, JsonSerializer.Serialize(s, _opt));
            try
            {
                System.IO.File.Replace(tmp, _file, null);
            }
            catch
            {
                System.IO.File.Move(tmp, _file, true);
            }
        }
        catch (Exception ex)
        {
            LogService.Error($"settings.json 写入失败（UI 状态未持久化）：{ex.Message}");
        }
    }
}
