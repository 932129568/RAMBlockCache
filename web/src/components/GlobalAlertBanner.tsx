import { Link } from 'react-router';
import { useQuery } from '@tanstack/react-query';
import { useDriverStore } from '@state/driver-store';
import { Icons } from './ui';

/**
 * [U2 2026-11-06] 全局异常面（status-first）：熔断/安全模式/丢脏/引导失败计数
 * 非零时在任意页面顶部置顶显示——操作型仪表盘纪律：异常优先于一切常规内容。
 * 复用 StatsPoller 的 ['rb','stats'] 查询（React Query 同键去重，零额外轮询）。
 * 仅在异常态渲染；正常态零 DOM、零开销。
 */
export default function GlobalAlertBanner() {
  const transport = useDriverStore((s) => s.transport);
  const { data: stats } = useQuery({
    queryKey: ['rb', 'stats'],
    queryFn: async () => {
      const result = await transport.getStats();
      if (!result.ok) throw new Error(result.error.message);
      return result.value;
    },
    retry: 0,
  });
  if (!stats) return null;

  const alerts: { tone: 'danger' | 'warning'; text: string; to: string; link: string }[] = [];
  if (stats.PanicActive === 1)
    alerts.push({ tone: 'danger', text: '熔断透传中：一切 I/O 直写盘，缓存未生效', to: '/emergency', link: '应急页' });
  if (stats.SafeMode === 1)
    alerts.push({ tone: 'warning', text: '安全模式（上次异常退出）：所有卷强制直写', to: '/emergency', link: '应急页' });
  if (stats.LostDirtyBytes > 0)
    alerts.push({
      tone: 'danger',
      text: `检测到丢失脏数据 ${stats.LostDirtyBytes} 字节——建议诊断页排查`,
      to: '/diagnostics',
      link: '诊断页',
    });
  if ((stats.BootFails ?? 0) > 0)
    alerts.push({ tone: 'warning', text: `引导失败计数 ${stats.BootFails}（boot_guard 记录）`, to: '/emergency', link: '应急页' });
  if (alerts.length === 0) return null;

  return (
    <div role="alert" className="mx-auto mt-2 max-w-6xl px-4" style={{ width: '100%' }}>
      {alerts.map((a) => {
        const fg = a.tone === 'danger' ? 'var(--rbc-danger-fg)' : 'var(--rbc-warning-fg)';
        const bg = a.tone === 'danger' ? 'var(--rbc-danger-bg)' : 'var(--rbc-warning-bg)';
        const bd = a.tone === 'danger' ? 'var(--rbc-danger-border)' : 'var(--rbc-warning-border)';
        return (
          <div
            key={a.text}
            className="mb-1 flex items-center gap-2 rounded-[var(--rbc-radius)] px-3 py-1.5 text-xs"
            style={{ color: fg, background: bg, border: `1px solid ${bd}` }}
          >
            <Icons.shield size={14} />
            <span className="font-medium">{a.text}</span>
            <span className="flex-1" />
            <Link to={a.to} className="underline underline-offset-2" style={{ color: fg }}>
              {a.link} →
            </Link>
          </div>
        );
      })}
    </div>
  );
}
