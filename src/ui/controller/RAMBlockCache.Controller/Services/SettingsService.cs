using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 控制台本地设置（序列化为 %LOCALAPPDATA%\RAMBlockCache\settings.json）。
/// JSON 键名 = 属性名（System.Text.Json 默认），重构期键名只读。
/// </summary>
public sealed class AppSettings
{
    /// <summary>按盘符（如 "D:"）保存的每卷配额（字节）。</summary>
    public Dictionary<string, ulong> QuotaByVolume { get; set; } = new();
    /// <summary>按盘符保存的每卷写模式（0=WriteThrough 1=WriteBack）。</summary>
    public Dictionary<string, uint> ModeByVolume { get; set; } = new();
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
}

/// <summary>settings.json 读写。读失败/损坏时回退默认设置，不抛。</summary>
public static class SettingsService
{
    private static readonly string _dir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RAMBlockCache");
    private static readonly string _file = Path.Combine(_dir, "settings.json");
    private static readonly JsonSerializerOptions _opt = new() { WriteIndented = true };

    /// <summary>读取设置；文件不存在或解析失败返回默认实例。</summary>
    public static AppSettings Load()
    {
        try { return JsonSerializer.Deserialize<AppSettings>(System.IO.File.ReadAllText(_file), _opt) ?? new(); }
        catch { return new(); }
    }

    /// <summary>写回 settings.json（自动创建目录）。</summary>
    public static void Save(AppSettings s)
    {
        Directory.CreateDirectory(_dir);
        System.IO.File.WriteAllText(_file, JsonSerializer.Serialize(s, _opt));
    }
}
