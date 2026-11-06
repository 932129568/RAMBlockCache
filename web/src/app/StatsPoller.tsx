import { useQuery, keepPreviousData } from '@tanstack/react-query';
import { useDriverStore } from '@state/driver-store';
import { recordStatsSample } from '@state/stats-history';

/**
 * 全局唯一统计轮询（1s）+ 卷枚举轮询（5s）。
 *
 * [FIX 2026-10-31] 统计查询从 StatusPage 上移至此：react-query 的去重只覆盖
 * "并发在飞"的请求，两个页面各自挂 refetchInterval 会形成双倍 IOCTL 节奏。
 * 挂在 App 后全应用只有这一路 1s 轮询，页面一律作为只读观察者消费缓存。
 * 卷枚举同理收编（5s：低频变化），加速页改为观察者 + 手动刷新。
 */
export default function StatsPoller() {
  const transport = useDriverStore((s) => s.transport);
  const reportError = useDriverStore((s) => s.reportError);
  const reportSuccess = useDriverStore((s) => s.reportSuccess);

  useQuery({
    queryKey: ['rb', 'stats'],
    queryFn: async () => {
      const result = await transport.getStats();
      if (!result.ok) {
        reportError(result.error);
        throw new Error(result.error.message);
      }
      reportSuccess();
      recordStatsSample(result.value);
      return result.value;
    },
    refetchInterval: 1000,
    refetchIntervalInBackground: false,
    retry: 0,
  });

  useQuery({
    queryKey: ['rb', 'volumes'],
    queryFn: async () => {
      const result = await transport.getVolumes();
      if (!result.ok) {
        // 卷枚举失败不污染全局连接态（统计轮询已负责上报），静默抛给观察者
        throw new Error(result.error.message);
      }
      return result.value;
    },
    refetchInterval: 5000,
    refetchIntervalInBackground: false,
    /* [PERF-UI 2026-11-04] 慢命令期间桥接排队 → 轮询超时不是故障：
     * 保留上一份数据继续显示（placeholderData）、失败不覆盖已知状态、
     * 重试一次。只有连续失败才会真正进入 error 态。 */
    retry: 1,
    retryDelay: 400,
    placeholderData: keepPreviousData,
  });

  return null;
}
