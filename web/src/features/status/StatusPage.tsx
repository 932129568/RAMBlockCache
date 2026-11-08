import { useQuery } from '@tanstack/react-query';
import type { RbStats, RbVolumeList } from '@protocol/types';
import { useDriverStore } from '@state/driver-store';
import StateBar from '@components/StateBar';
import { AnimatedValue, Badge, Donut, Metric, PageHeader, Skeleton, SkeletonCard } from '@components/ui';

/** 命中率以字节为准而非次数：与驱动统计口径一致 */
function hitRateOf(readBytes: number, readHitBytes: number): number | null {
  if (readBytes <= 0) return null;
  return (readHitBytes / readBytes) * 100;
}

/**
 * 状态页（Bento 布局）：
 *  - 主视觉：字节命中率大环（弹簧数值）+ 省下磁盘读
 *  - StateBar 四段横贯
 *  - KPI 条 + 延迟双卡（EWMA 条形对比，未采样显式标注）
 */
export default function StatusPage() {
  const phase = useDriverStore((s) => s.phase);
  const transport = useDriverStore((s) => s.transport);
  /* [UI-UP 2026-11-05] 驱动版本卡：GET_VERSION 此前在传输层存在但零消费——
   * 部署验证反复需要确认运行版本/签名者/测试签名态，接入为状态页常驻卡片。 */
  const versionQuery = useQuery({
    queryKey: ['rb', 'version'],
    queryFn: async () => {
      const r = await transport.getVersion();
      if (!r.ok) throw new Error(r.error.message);
      return r.value;
    },
    refetchInterval: 30000,
    retry: 1,
  });
  const statsQuery = useQuery<RbStats>({
    queryKey: ['rb', 'stats'],
    enabled: false,
    retry: 0,
  });
  const volumesQuery = useQuery<RbVolumeList>({
    queryKey: ['rb', 'volumes'],
    enabled: false,
    retry: 0,
  });
  const mounted = (volumesQuery.data?.Items ?? []).filter((v) => v.Attached === 1).length;
  const totalVolumes = volumesQuery.data?.Count ?? null;

  if (statsQuery.isPending) {
    return (
      <div className="flex flex-col gap-4" aria-busy>
        <SkeletonCard rows={2} />
        <div className="grid grid-cols-4 gap-3">
          {[0, 1, 2, 3].map((i) => (
            <div key={i} className="rbc-card !p-3">
              <Skeleton w={"55%"} h={10} />
              <div className="h-2" />
              <Skeleton w={"40%"} h={22} />
            </div>
          ))}
        </div>
      </div>
    );
  }

  if (statsQuery.isError) {
    return (
      <div
        role="alert"
        className="rbc-card flex flex-col items-start gap-2 p-5"
        style={{ borderColor: 'var(--rbc-danger-border)' }}
      >
        <div className="text-sm" style={{ color: 'var(--rbc-danger-fg)' }}>
          统计读取失败
        </div>
        <div className="text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
          {(statsQuery.error as Error).message}
        </div>
      </div>
    );
  }

  const stats = statsQuery.data;
  const hitRate = hitRateOf(stats.ReadBytes, stats.ReadHitBytes);
  const panicking = stats.PanicActive === 1;
  const servedMB = stats.ReadHitBytes / 1048576;
  const wbMB = stats.WbBytes / 1048576;
  const cacheMB = stats.CurBytes / 1048576;
  const dirtyMB = stats.DirtyBytes / 1048576;

  return (
    <div className="flex flex-col gap-4">
      <PageHeader
        title="运行状态"
        desc="缓存实时状态与健康监控"
        actions={
          <Badge tone={panicking ? 'warn' : stats.Enabled === 1 ? 'ok' : 'neutral'}>
            {panicking ? '全局直通' : stats.Enabled === 1 ? '缓存生效中' : '旁路待命'}
          </Badge>
        }
      />

      <StateBar
        phase={phase}
        enabled={stats.Enabled === 1}
        panic={stats.PanicActive === 1}
        safeMode={stats.SafeMode === 1}
      />

      {/* [UI-UP 2026-11-05] 驱动版本卡：部署验证的核心信息（构建哈希需日志配合，
          此处展示协议版本/签名者/测试签名态——GET_VERSION 的三个关键字段）。 */}
      <DriverVersionCard v={versionQuery.data} />

      <p className="rbc-fade-up text-sm" style={{ color: 'var(--rbc-text-secondary)' }}>
        {mounted > 0
          ? `缓存正在保护 ${mounted} 个卷` + (totalVolumes !== null ? `（共枚举 ${totalVolumes} 卷）` : '')
          : '当前没有卷在加速。到「加速」页热开启一个数据卷开始收集收益。'}
      </p>

      {panicking && (
        <div
          role="alert"
          className="rbc-fade-up rounded-[var(--rbc-radius-md)] p-4 text-sm"
          style={{
            background: 'var(--rbc-warning-bg)',
            border: '1px solid var(--rbc-warning-border)',
            color: 'var(--rbc-warning-fg)',
          }}
        >
          全局熔断中：所有 I/O 直通磁盘，缓存统计仍在累计但不再命中。
        </div>
      )}

      {/* —— Bento：主视觉行 —— */}
      <div className="grid grid-cols-3 gap-4">
        <div className="rbc-card rbc-fade-up col-span-1 flex items-center justify-center gap-2">
          <Donut
            percent={hitRate}
            label="字节命中率"
            sub={hitRate === null ? '尚无读流量' : undefined}
          />
        </div>
        <div className="rbc-card rbc-fade-up-late col-span-2 flex flex-col justify-center gap-4 p-5">
          <div className="flex items-baseline gap-2">
            <span className="text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
              本次开机已省下磁盘读
            </span>
            <span className="font-mono text-3xl font-semibold rbc-value-accent">
              <AnimatedValue value={servedMB} format={(n) => n.toFixed(1)} />
            </span>
            <span className="text-sm" style={{ color: 'var(--rbc-text-tertiary)' }}>
              MB
            </span>
          </div>
          <div className="grid grid-cols-3 gap-4 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
            <div>
              <div className="font-mono text-lg" style={{ color: 'var(--rbc-text-primary)' }}>
                {stats.ReadHit.toLocaleString('zh-CN')}
              </div>
              <div>命中读次数</div>
            </div>
            <div>
              <div className="font-mono text-lg" style={{ color: 'var(--rbc-text-primary)' }}>
                {stats.ReadReq.toLocaleString('zh-CN')}
              </div>
              <div>总读请求</div>
            </div>
            <div>
              <div className="font-mono text-lg" style={{ color: 'var(--rbc-text-primary)' }}>
                {(stats.ReadBytes / 1073741824).toFixed(2)}
              </div>
              <div>总读 GB</div>
            </div>
          </div>
        </div>
      </div>

      {/* —— KPI 条 —— */}
      <div className="rbc-fade-up grid grid-cols-5 gap-3">
        <Metric label="当前缓存" value={`${cacheMB.toFixed(1)} MB`} />
        <Metric label="脏数据" value={`${dirtyMB.toFixed(1)} MB`} />
        <Metric label="生效上限" value={`${stats.EffectiveCacheMB} MB`} hint={stats.EffectiveCacheMB === 0 ? '引擎未创建' : undefined} />
        <Metric label="回写落盘" value={`${wbMB.toFixed(1)} MB`} hint={`回写 ${stats.WbFlush.toLocaleString('zh-CN')} 次`} />
        {/* [F2 2026-11-06] TRIM 计数：驱动 rbctl.c RbTrimRangeInvalidate 处理的裁剪次数 */}
        <Metric label="TRIM 已处理" value={`${stats.TrimCount.toLocaleString('zh-CN')} 次`} />
      </div>

      {/* —— 延迟双卡（EWMA 条形对比 + U1 分位数明细）—— */}
      <div className="rbc-fade-up grid grid-cols-2 gap-3">
        {(
          [
            { label: '命中读延迟', us: stats.CacheReadLatencyUs, color: 'var(--rbc-chart-1)', ref: 500, p50: stats.CacheReadLatP50Us, p95: stats.CacheReadLatP95Us, p99: stats.CacheReadLatP99Us },
            { label: '下盘读延迟', us: stats.DiskReadLatencyUs, color: 'var(--rbc-chart-2)', ref: 2000, p50: stats.DiskReadLatP50Us, p95: stats.DiskReadLatP95Us, p99: stats.DiskReadLatP99Us },
          ] as const
        ).map((d) => (
          <div key={d.label} className="rbc-card !p-3">
            <div className="flex items-baseline justify-between">
              <span className="text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
                {d.label}
              </span>
              <span className="font-mono text-lg" style={{ color: 'var(--rbc-text-primary)' }}>
                {d.us === 0 ? '未采样' : `${d.us.toLocaleString('zh-CN')} µs`}
              </span>
            </div>
            <div
              className="mt-2 h-1.5 rounded-full"
              style={{ background: 'var(--rbc-track-bg)' }}
              role="presentation"
            >
              {d.us > 0 && (
                <div
                  className="h-1.5 rounded-full"
                  style={{
                    width: `${Math.min(100, (d.us / d.ref) * 100)}%`,
                    background: d.color,
                    transition: 'width var(--rbc-dur-320) ease',
                  }}
                />
              )}
            </div>
            {d.us === 0 && (
              <div className="mt-1 text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
                尚未采到样本：延迟按 1/64 抽样（每 64 笔读采 1 笔），多读写一些即出数；不画假条
              </div>
            )}
            {/* [U1 2026-11-06] 分位数明细（log2 桶近似；0=尚无样本不画假值） */}
            <div className="mt-1 flex items-baseline justify-between text-xs">
              <span style={{ color: 'var(--rbc-text-tertiary)' }}>P50 / P95 / P99</span>
              <span className="font-mono" style={{ color: 'var(--rbc-text-secondary)' }}>
                {d.p50 === 0
                  ? '—（分位无样本）'
                  : `P50 ${d.p50.toLocaleString('zh-CN')} · P95 ${d.p95.toLocaleString('zh-CN')} · P99 ${d.p99.toLocaleString('zh-CN')} µs`}
              </span>
            </div>
          </div>
        ))}
      </div>

      <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
        每 1s 轮询；切到后台标签页自动暂停。延迟为 EWMA + 分位数（log2 桶近似），0 表示驱动尚无样本。
      </p>
    </div>
  );
}


/* [UI-UP 2026-11-05] 驱动版本卡：常驻显示运行版本/签名者/测试签名态。
 * 部署验证（换机/升级后确认"跑的是哪个构建"）此前只能靠命令行。 */
function DriverVersionCard({ v }: {
  v: { Major: number; Minor: number; Build: number; TestSigned: number; Signer: string } | undefined;
}) {
  if (!v) return null;
  const ver = `v${v.Major}.${v.Minor}.${v.Build}`;
  const test = v.TestSigned === 1;
  return (
    <div className="rbc-card p-4 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
      <span style={{ fontWeight: 600, color: 'var(--rbc-text-primary)' }}>驱动 {ver}</span>
      <span style={{ margin: '0 8px' }}>·</span>
      签名者 {v.Signer || '（未提供）'}
      <span style={{ margin: '0 8px' }}>·</span>
      {test ? (
        <span style={{ color: 'var(--rbc-warning-fg)' }}>测试签名</span>
      ) : (
        <span style={{ color: 'var(--rbc-ok-fg)' }}>正式签名</span>
      )}
    </div>
  );
}
