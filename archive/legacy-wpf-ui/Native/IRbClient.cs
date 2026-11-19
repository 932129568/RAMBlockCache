using System.Threading.Tasks;
using RamBoostFS.Wpf.Models;

namespace RamBoostFS.Wpf.Native;

public interface IRbClient
{
    bool IsConnected { get; }
    string BackendName { get; }

    Task<RbStats?> GetStatsAsync();
    Task<RbConfig?> GetConfigAsync();
    Task<bool> SetConfigAsync(RbConfig cfg);
    Task<bool> FlushAsync();
    Task<bool> ExitSafeModeAsync();

    /// <summary>设置单卷读/写开关与写模式 (RB_WRITEMODE_*)。</summary>
    Task<bool> SetVolumeAsync(char letter, bool read, bool write, uint writeMode);
    Task<RbVolumeList?> GetVolumesAsync();

    /// <summary>运行时热开关某卷缓存；writeMode 为启用时应用的写模式，传 RB_WRITEMODE_GLOBAL=2 表示不变。</summary>
    Task<bool> AttachVolumeAsync(char letter, bool attach, bool read, bool write, uint writeMode);

    Task<RbLogList?> GetLogListAsync(uint cursor);

    /// <summary>V8 新增: 协议/驱动版本，兼作连通性自检 (RB_MSG_GET_VERSION=10)。</summary>
    Task<RbVersion?> GetVersionAsync();
}
