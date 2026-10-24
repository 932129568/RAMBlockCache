using System.Runtime.InteropServices;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// 电源状态探测（F3 回写引导的前置输入；P/Invoke GetSystemPowerStatus，零外部依赖——
/// WPF 共享框架无 System.Management/PowerStatus，故走 kernel32）。
/// 06 契约 #10：电源快照 {batteryPresent, powerStatus, chargePercent}。
/// 三态判定（05 F3）：台式机（无系统电池）/ 笔记本（有电池）/ 电池低电量（≤30%）。
/// 封送纪律：byte[] 直读（marshaler pin 数组传指针，最简封送零结构体布局风险）——
/// 原 ref struct 实现在 x64 上踩过 AccessViolation（15:2x 崩溃根因二）；
/// 且本方法整体 try/catch + 失败按"未知"保守处理（F3 话术已覆盖未知态）。
/// </summary>
public static class PowerInfo
{
    [DllImport("kernel32.dll")]
    private static extern bool GetSystemPowerStatus([Out] byte[] lpSystemPowerStatus);

    /// <summary>电源快照（探测失败按"未知"保守处理——F3 引导话术已覆盖未知态）。</summary>
    public sealed record PowerSnapshot(bool BatteryPresent, int ChargePercent, bool OnAC);

    /// <summary>未知快照（保守：按无电池台式机处理，回写引导不误判低电量）。</summary>
    public static PowerSnapshot Unknown { get; } = new(false, -1, true);

    /// <summary>读取当前电源快照（任何失败=Unknown，绝不抛）。</summary>
    public static PowerSnapshot Query()
    {
        var buf = new byte[12];   // native SYSTEM_POWER_STATUS=4 字节；多给不读（marshaler 按数组长 pin）
        try
        {
            if (!GetSystemPowerStatus(buf)) return Unknown;
            byte ac = buf[0];      // 0=电池供电 1=AC 供电 255=未知
            byte flag = buf[1];    // 1=高 2=低 4=危急 8=充电 128=无电池
            byte pct = buf[2];     // 0-100，255=未知
            bool noBattery = (flag & 128) != 0;
            bool onAC = ac == 1;
            int charge = pct <= 100 ? pct : -1;
            return new PowerSnapshot(!noBattery, charge, onAC);
        }
        catch
        {
            return Unknown;
        }
    }

    /// <summary>F3 电源态键（VM PowerState 消费）：desktop / laptop / low / unknown。</summary>
    public static string StateKey(PowerSnapshot s)
    {
        if (!s.BatteryPresent) return "desktop";
        if (s.ChargePercent is >= 0 and <= 30) return "low";
        return "laptop";
    }
}
