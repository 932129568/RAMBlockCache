using System;

namespace RAMBlockCache.Controller.Services;

/// <summary>
/// [契约 B-9 用户态实现] 挂载前重复盘符/卷冲突校验。
/// 形状出处：09 移交报告 §4 B-9「挂栈前校验卷 GUID/盘符唯一性；冲突返回明确错误码
/// （前端展示部分已就绪）」。
///
/// **实现边界（为什么是用户态）**：权威校验应在驱动挂栈路径内（拒绝第二个设备挂同
/// 一盘符），协议/驱动侧实现未就绪；本类做**前置可读化**——在挂载 IOCTL 下发前用
/// VOLSTATUS（0x905）拿当前挂载盘符集合，目标盘符已在集合即预拒，把驱动侧的
/// "重复挂载"错误码提前翻译成用户可读原因（不再只看到裸 IOCTL 失败）。
/// 降级语义（契约 DTO 注释口径）：VOLSTATUS 查询失败=校验不可用→**放行**
/// （由驱动继续做权威拒绝，不新增阻塞面）。
/// </summary>
public static class VolumeGuard
{
    /// <summary>校验结果（Ok=false 时 ErrorText 为可读原因；ErrorCode 供日志归因）。</summary>
    public sealed record Result(bool Ok, string? ErrorCode, string? ErrorText)
    {
        /// <summary>校验不可用降级=放行（驱动继续权威拒绝）。</summary>
        public static Result Pass() => new(true, null, null);
    }

    /// <summary>
    /// 挂载前校验：目标盘符是否已被缓存驱动挂载。
    /// </summary>
    /// <param name="driveLetter">待挂载盘符（如 'D'）。</param>
    public static Result CheckMount(char driveLetter)
    {
        try
        {
            var accel = DriverService.Instance.QueryVolumeAccel();
            char up = char.ToUpperInvariant(driveLetter);
            foreach (var v in accel)
            {
                if (char.ToUpperInvariant(v.Letter) == up)
                {
                    return new Result(false, "DuplicateVolume",
                        $"盘符 {up}: 已挂载在缓存驱动上。如为重复挂载，请先卸载该卷再试；"
                        + "若盘符被其他卷设备占用，请先在磁盘管理中调整。");
                }
            }
            return Result.Pass();
        }
        catch (Exception ex)
        {
            // 校验不可用=放行（禁把探测失败升级成挂载阻塞——驱动仍会权威拒绝）
            LogService.Warn($"[B-9] 挂载前盘符校验不可用（放行由驱动权威拒绝）：{ex.Message}");
            return Result.Pass();
        }
    }
}
