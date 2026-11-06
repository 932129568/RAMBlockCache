import { useEffect, useRef } from 'react';
import { useQuery } from '@tanstack/react-query';
import * as echarts from 'echarts/core';
import { LineChart, HeatmapChart } from 'echarts/charts';
import { GridComponent, TooltipComponent, VisualMapComponent } from 'echarts/components';
import { CanvasRenderer } from 'echarts/renderers';
import type { EChartsCoreOption } from 'echarts/core';
import type { RbStats } from '@protocol/types';
import { getStatsHistory, hasReadTraffic, hasWriteTraffic, type StatsSample } from '@state/stats-history';
import { ensureRbcTheme } from '@lib/echarts-theme';
import { Card, Metric, PageHeader } from '@components/ui';

echarts.use([LineChart, HeatmapChart, GridComponent, TooltipComponent, VisualMapComponent, CanvasRenderer]);

/** 图表色从设计令牌取（tokens.css 是唯一事实源）；canvas 不受 forced-colors 覆盖，由相邻文本卡承语义 */
function token(name: string, fallback: string): string {
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return v || fallback;
}

function fmtMB(bytes: number): string {
  return (bytes / 1024 / 1024).toFixed(1);
}

function fmtTime(t: number): string {
  return new Date(t).toLocaleTimeString('zh-CN', { hour12: false });
}

/** echarts 挂载/随 option 更新/随容器缩放的通用壳 */
function Chart({
  option,
  height,
  label,
}: {
  option: EChartsCoreOption;
  height: number;
  label: string;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const chartRef = useRef<ReturnType<typeof echarts.init> | null>(null);

  useEffect(() => {
    if (!ref.current) return;
    ensureRbcTheme();
    const chart = echarts.init(ref.current, 'rbc');
    chartRef.current = chart;
    const ro = new ResizeObserver(() => chart.resize());
    ro.observe(ref.current);
    return () => {
      ro.disconnect();
      chart.dispose();
      chartRef.current = null;
    };
  }, []);

  useEffect(() => {
    chartRef.current?.setOption(option);
  }, [option]);

  return <div ref={ref} role="img" aria-label={label} style={{ height }} />;
}

function axisOf(): { axisLine: object; axisLabel: object; splitLine: object } {
  return {
    axisLine: { lineStyle: { color: token('--rbc-chart-grid', '#23232b') } },
    axisLabel: { color: token('--rbc-chart-axis-text', '#9a9aa5') },
    splitLine: { lineStyle: { color: token('--rbc-chart-grid', '#23232b') } },
  };
}

/**
 * 收益页（§11.5 迁移顺序第 3 页）。
 * 口径纪律：一切曲线来自 1s 真实采样的差值（stats-history.ts），
 * 无读流量时整页显式空态——不画 0 曲线冒充收益（对应 WPF 诚实降级约定）。
 */
export default function BenefitPage() {
  const statsQuery = useQuery<RbStats>({ queryKey: ['rb', 'stats'], enabled: false, retry: 0 });
  const stats: RbStats | undefined = statsQuery.data;
  const history = getStatsHistory();
  const traffic = hasReadTraffic();
  const writeTraffic = hasWriteTraffic();

  const hitRate = stats && stats.ReadBytes > 0
    ? (stats.ReadHitBytes / stats.ReadBytes) * 100
    : null;
  const savedReqs = stats ? stats.ReadHit : 0;

  const hitSeries = history.map((s) => [fmtTime(s.t), s.hitRate]);
  const rateSeries = history.map((s) => [fmtTime(s.t), s.hitMBps, s.diskMBps]);

  // 分片热力：取最近一个有流量的采样（StripeRead 累计差值 → 8×8 网格）
  const lastActive: StatsSample | undefined = [...history].reverse().find(
    (s) => s.stripeRate.some((v) => v > 0) || s.stripeWriteRate.some((v) => v > 0),
  );
  const heatData: Array<[number, number, number]> = [];
  if (lastActive) {
    lastActive.stripeRate.forEach((v, i) => {
      heatData.push([Math.floor(i / 8), i % 8, Number(v.toFixed(2))]);
    });
  }
  const heatMax = heatData.length ? Math.max(...heatData.map((d) => d[2])) : 1;

  return (
    <div className="flex flex-col gap-4">
      <PageHeader
        title="缓存收益"
        desc="命中率 / 吞吐 / 分片热力——一切曲线来自 1s 真实采样差值，空态不画假图"
      />
      {/* [UI-UP] 账单叙事头条：把四个数字合成一句话结论（设计锚点=成绩单） */}
      {stats && hitRate !== null && (
        <div
          className="rbc-fade-up rounded-[var(--rbc-radius-md)] p-4"
          style={{ background: 'linear-gradient(90deg, var(--rbc-ok-bg, #ECFDF5), transparent)', border: '1px solid var(--rbc-ok-border, #A7F3D0)' }}
        >
          <span style={{ fontSize: 15, color: 'var(--rbc-text-primary)' }}>
            本开机已读 <b>{fmtMB(stats.ReadBytes)}</b>，其中 <b>{fmtMB(stats.ReadHitBytes)}</b> 直接来自内存 ——{' '}
            相当于替这块盘扛下了 <b>{savedReqs.toLocaleString('zh-CN')}</b> 次寻道。
          </span>
        </div>
      )}
      <div className="grid grid-cols-4 gap-3">
        <Metric label="累计读流量" value={stats ? fmtMB(stats.ReadBytes) : '—'} />
        <Metric label="命中免下盘" value={stats ? fmtMB(stats.ReadHitBytes) : '—'} />
        <Metric
          label="字节命中率"
          value={hitRate === null ? '—' : `${hitRate.toFixed(1)}%`}
          hint={hitRate === null ? '尚无读流量' : undefined}
        />
        <Metric label="命中读次数" value={savedReqs.toLocaleString('zh-CN')} />
      </div>

      {/* 占用环 + 双条对比：独立于读流量门（缓存占用在直写+只写场景也成立） */}
      <div className="grid grid-cols-2 gap-3">
        <Card title="缓存占用环（CurBytes / 生效上限）">
          <UsageRing
            usedBytes={stats?.CurBytes ?? 0}
            limitMB={stats?.EffectiveCacheMB ?? 0}
            autoLimitMB={stats?.DynamicLimitMB ?? 0}
            availableMB={stats?.AvailablePhysMB ?? 0}
          />
        </Card>
        <Card title="最近采样读写对比（MB/s）">
          <DualBarCompare
            hitMBps={lastActive?.hitMBps ?? history[history.length - 1]?.hitMBps ?? 0}
            diskMBps={lastActive?.diskMBps ?? history[history.length - 1]?.diskMBps ?? 0}
            writeMBps={history[history.length - 1]?.writeMBps ?? 0}
          />
        </Card>
      </div>

      {!traffic ? (
        <section
          role="status"
          className="rounded-[var(--rbc-radius-md)] p-8 text-center text-sm"
          style={{
            background: 'var(--rbc-bg-surface)',
            border: '1px dashed var(--rbc-border-strong)',
            color: 'var(--rbc-text-tertiary)',
          }}
        >
          尚无读流量：真实收益曲线（命中率 / 吞吐对比 / 分片热力）会在
          挂载卷并产生读 I/O 后出现。空态不画 0 曲线冒充收益。
        </section>
      ) : (
        <>
          <Card title="字节命中率趋势（2 分钟窗口）">
            <Chart
              label="字节命中率趋势折线图"
              height={200}
              option={{
                animation: false,
                grid: { left: 48, right: 16, top: 16, bottom: 52 },
                dataZoom: [
                  { type: 'inside' },
                  { type: 'slider', height: 16, bottom: 8 },
                ],
                xAxis: { type: 'category', data: hitSeries.map((d) => d[0]), ...axisOf() },
                yAxis: { type: 'value', max: 100, ...axisOf() },
                series: [
                  {
                    type: 'line',
                    data: hitSeries.map((d) => d[1]),
                    connectNulls: false, // 无流量区间断线，不画 0 冒充
                    showSymbol: false,
                    lineStyle: { color: token('--rbc-chart-1', '#4cc2ff') },
                    areaStyle: { color: token('--rbc-chart-area-fill', '#1f4cc2ff') },
                  },
                ],
              }}
            />
          </Card>

          <Card title="读吞吐对比：命中 vs 下盘（MB/s）">
            <Chart
              label="命中读与下盘读吞吐对比折线图"
              height={220}
              option={{
                animation: false,
                grid: { left: 48, right: 16, top: 32, bottom: 28 },
                legend: {
                  top: 0,
                  textStyle: { color: token('--rbc-chart-axis-text', '#9a9aa5') },
                },
                xAxis: { type: 'category', data: rateSeries.map((d) => d[0]), ...axisOf() },
                yAxis: { type: 'value', ...axisOf() },
                series: [
                  {
                    name: '命中读',
                    type: 'line',
                    data: rateSeries.map((d) => d[1]),
                    showSymbol: false,
                    lineStyle: { color: token('--rbc-chart-1', '#4cc2ff') },
                  },
                  {
                    name: '下盘读',
                    type: 'line',
                    data: rateSeries.map((d) => d[2]),
                    showSymbol: false,
                    lineStyle: { color: token('--rbc-chart-2', '#2e86d6') },
                  },
                ],
              }}
            />
          </Card>

          <Card title="分片读热力（64 stripe × 最近活跃采样，MB/s）">
            <Chart
              label="64 分片读速率热力图，8×8 网格"
              height={200}
              option={{
                animation: false,
                grid: { left: 16, right: 64, top: 8, bottom: 24 },
                tooltip: { show: true },
                visualMap: {
                  min: 0,
                  max: heatMax,
                  calculable: false,
                  orient: 'vertical',
                  right: 0,
                  top: 'center',
                  inRange: {
                    color: [
                      token('--rbc-chart-heat-1', '#1e3a4d'),
                      token('--rbc-chart-heat-2', '#2e86d6'),
                      token('--rbc-chart-heat-3', '#4cc2ff'),
                    ],
                  },
                  textStyle: { color: token('--rbc-chart-axis-text', '#9a9aa5') },
                },
                xAxis: { type: 'category', show: false },
                yAxis: { type: 'category', show: false },
                series: [
                  {
                    type: 'heatmap',
                    data: heatData,
                    label: { show: false },
                  },
                ],
              }}
            />
          </Card>
        </>
      )}

      {/* 写吞吐：独立空态门（只写工作负载也能看到写曲线） */}
      <Card title="写吞吐（MB/s）：总写 vs 进缓存">
        {writeTraffic ? (
          <Chart
            label="写吞吐折线图：总写与进缓存"
            height={180}
            option={{
              animation: false,
              grid: { left: 48, right: 16, top: 32, bottom: 28 },
              legend: {
                top: 0,
                textStyle: { color: token('--rbc-chart-axis-text', '#9a9aa5') },
              },
              xAxis: {
                type: 'category',
                data: history.map((s) => fmtTime(s.t)),
                ...axisOf(),
              },
              yAxis: { type: 'value', ...axisOf() },
              series: [
                {
                  name: '总写',
                  type: 'line',
                  data: history.map((s) => Number(s.writeMBps.toFixed(2))),
                  showSymbol: false,
                  lineStyle: { color: token('--rbc-chart-2', '#2e86d6') },
                },
                {
                  name: '进缓存',
                  type: 'line',
                  data: history.map((s) => Number(s.cachedWriteMBps.toFixed(2))),
                  showSymbol: false,
                  lineStyle: { color: token('--rbc-chart-1', '#4cc2ff') },
                },
              ],
            }}
          />
        ) : (
          <p className="py-4 text-center text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
            尚无写流量。直写模式下「进缓存」恒为 0 是诚实值（写直落盘），
            回写模式写缓存标脏即计。
          </p>
        )}
      </Card>

      <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
        口径：字节命中率以字节为准（与驱动统计一致）；吞吐/热力为 1s 采样差值，
        计数器回退（驱动重启）记 0 不记负；延迟明细见状态页。
      </p>
    </div>
  );
}

/** 占用环：CurBytes / EffectiveCacheMB；上限未采样（0）时显式说明，不画 0% 假环 */
function UsageRing({
  usedBytes,
  limitMB,
  autoLimitMB = 0,
  availableMB = 0,
}: {
  usedBytes: number;
  limitMB: number;
  /** 动态预算（AutoMemory=1 时 >0），用于标注上限来源 */
  autoLimitMB?: number;
  availableMB?: number;
}) {
  const usedMB = usedBytes / 1024 / 1024;
  const pct = limitMB > 0 ? Math.min(100, (usedMB / limitMB) * 100) : null;
  const R = 52;
  const C = 2 * Math.PI * R;
  return (
    <div className="flex items-center gap-4">
      <svg width={128} height={128} role="img" aria-label={`缓存占用环，${pct === null ? '上限未采样' : `占用 ${pct.toFixed(1)}%`}`}>
        <circle cx={64} cy={64} r={R} fill="none" stroke="var(--rbc-chart-track)" strokeWidth={10} />
        {pct !== null && pct > 0 && (
          <circle
            cx={64}
            cy={64}
            r={R}
            fill="none"
            stroke="var(--rbc-chart-1)"
            strokeWidth={10}
            strokeLinecap="round"
            strokeDasharray={`${(pct / 100) * C} ${C}`}
            transform="rotate(-90 64 64)"
          />
        )}
        <text x={64} y={60} textAnchor="middle" fill="var(--rbc-text-primary)" fontSize={20} fontFamily="monospace">
          {pct === null ? '—' : `${pct.toFixed(0)}%`}
        </text>
        <text x={64} y={80} textAnchor="middle" fill="var(--rbc-text-tertiary)" fontSize={11}>
          {usedMB.toFixed(1)} MB
        </text>
      </svg>
      <div className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
        <div>
          占用 <span className="font-mono">{usedMB.toFixed(1)} MB</span>
        </div>
        <div className="mt-1">
          生效上限{' '}
          <span className="font-mono">
            {limitMB > 0 ? `${limitMB} MB` : '未采样（引擎未创建）'}
          </span>
          {/* [UI-FIX 2026-11-05] 自动预算模式下把"动态上限"与"可用物理内存"
              一并显示：否则用户只看到生效上限变化却不知从何而来。 */}
          {autoLimitMB > 0 && (
            <span style={{ marginLeft: 6, color: 'var(--rbc-accent)' }}>
              动态（可用 {availableMB} MB）
            </span>
          )}
        </div>
        {limitMB > 0 && usedMB > limitMB * 0.9 && (
          <div className="mt-1" style={{ color: 'var(--rbc-warning-fg)' }}>
            接近上限，驱逐将加剧
          </div>
        )}
      </div>
    </div>
  );
}

/** 双条对比：最近采样 命中/下盘/总写 三条水平条（WPF DualBarCompare 语义） */
function DualBarCompare({
  hitMBps,
  diskMBps,
  writeMBps,
}: {
  hitMBps: number;
  diskMBps: number;
  writeMBps: number;
}) {
  const rows: Array<{ label: string; v: number; color: string }> = [
    { label: '命中读', v: hitMBps, color: 'var(--rbc-chart-1)' },
    { label: '下盘读', v: diskMBps, color: 'var(--rbc-chart-2)' },
    { label: '总写', v: writeMBps, color: 'var(--rbc-chart-heat-3)' },
  ];
  const max = Math.max(...rows.map((r) => r.v), 1);
  return (
    <div className="flex flex-col gap-3">
      {rows.map((r) => (
        <div key={r.label}>
          <div className="mb-1 flex justify-between text-xs">
            <span style={{ color: 'var(--rbc-text-secondary)' }}>{r.label}</span>
            <span className="font-mono" style={{ color: 'var(--rbc-text-primary)' }}>
              {r.v.toFixed(2)} MB/s
            </span>
          </div>
          <div style={{ height: 8, background: 'var(--rbc-chart-track)', borderRadius: 4 }}>
            <div
              style={{
                height: 8,
                width: `${(r.v / max) * 100}%`,
                background: r.color,
                borderRadius: 4,
                transition: 'width var(--rbc-dur-320) ease',
              }}
            />
          </div>
        </div>
      ))}
      {hitMBps === 0 && diskMBps === 0 && writeMBps === 0 && (
        <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
          尚无 I/O 采样（最近 1s）。
        </p>
      )}
    </div>
  );
}

