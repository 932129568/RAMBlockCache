import { useEffect, useRef, type ReactNode } from 'react';
import { useQuery } from '@tanstack/react-query';
import * as echarts from 'echarts/core';
import { LineChart, HeatmapChart } from 'echarts/charts';
import {
  DataZoomComponent,
  GridComponent,
  LegendComponent,
  TooltipComponent,
  VisualMapComponent,
} from 'echarts/components';
import { CanvasRenderer } from 'echarts/renderers';
import type { EChartsCoreOption } from 'echarts/core';
import type { RbStats } from '@protocol/types';
import { getStatsHistory, hasReadTraffic, hasWriteTraffic, type StatsSample } from '@state/stats-history';
import { ensureRbcTheme } from '@lib/echarts-theme';
import { useMotion } from '@lib/motion';
import { AnimatedValue, Badge, Card, Metric, PageHeader } from '@components/ui';

/* [UI-2026-11-12 真缺陷] echarts 6 的 core 是**按需注册**：没 use() 的组件不会报错，
 * 直接静默不渲染。此前这里只注册了 Line/Heatmap/Grid/Tooltip/VisualMap，
 * 于是：
 *   - `legend` 两系列图（读吞吐对比 / 写吞吐）的图例**从来没出现过**——
 *     读者无法分辨哪条线是"命中读"、哪条是"下盘读"，多系列图退化成两根无标注曲线；
 *   - `dataZoom` 的 slider 也不渲染，grid.bottom 预留的 52px 成了空白。
 * 二者都不影响 tsc / vite build / 运行时无异常，只有肉眼看截图才发现。
 * 教训：tree-shaking 版 echarts 的 option 字段缺失是"静默失败"，必须按图表
 * 实际用到的组件清单逐项核对。 */
echarts.use([
  LineChart,
  HeatmapChart,
  DataZoomComponent,
  GridComponent,
  LegendComponent,
  TooltipComponent,
  VisualMapComponent,
  CanvasRenderer,
]);

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
  /* [A8 2026-11-11] 入场动效只在**首帧**播放一次。
   *
   * 原实现全部 4 张图都写死 `animation: false`——原因大概率是担心
   * 每秒采样刷新时动画反复重播导致抖动。但代价是"图表直接跳出来"，
   * 数据从 0 到满屏没有任何叙事。
   *
   * 正确解法是区分**首次绘制**与**增量更新**：
   *   - 首次：animationDuration 420ms，线条从左向右生长（echarts 原生
   *     的 clip 动画），让"这里有一条趋势"这件事本身被看见；
   *   - 更新：animationDurationUpdate 0 —— 数据每秒变一次，任何补间都会
   *     变成永久抖动的噪声。用户要看的是**当前值**，不是动画。
   * 减弱动效时两者都置 0（首帧也直接到位）。 */
  const firstRef = useRef(true);
  const { reduce } = useMotion();

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
      firstRef.current = true;
    };
  }, []);

  useEffect(() => {
    const chart = chartRef.current;
    if (!chart) return;
    const first = firstRef.current;
    firstRef.current = false;
    chart.setOption(
      {
        ...option,
        animation: first && !reduce,
        animationDuration: first && !reduce ? 420 : 0,
        animationDurationUpdate: 0,
        animationEasing: 'cubicOut',
      },
      { notMerge: false, lazyUpdate: true },
    );
  }, [option, reduce]);

  return <div ref={ref} role="img" aria-label={label} style={{ height }} />;
}

function axisOf(): { axisLine: object; axisLabel: object; splitLine: object } {
  return {
    axisLine: { lineStyle: { color: token('--rbc-chart-grid', '#23232b') } },
    axisLabel: { color: token('--rbc-chart-axis-text', '#9a9aa5') },
    splitLine: { lineStyle: { color: token('--rbc-chart-grid', '#23232b') } },
  };
}

/** 图内空态：说明"为什么这里是空的 + 怎么让它有数据"，不画 0 曲线冒充收益 */
function ChartEmpty({ children }: { children: ReactNode }) {
  return (
    <p className="py-6 text-center text-xs" style={{ color: 'var(--rbc-text-tertiary)', lineHeight: 1.7 }}>
      {children}
    </p>
  );
}

/**
 * 收益页（§11.5 迁移顺序第 3 页）—— 2026-11-12 五段式重构
 *
 * 本页在全站的**唯一职分**是回答「攒了多少、怎么变的」。
 * 因此刻意不重复状态页的两块内容：
 *   - 不再是"占用环"（首屏最抢眼的环）→ 占用率/生效上限属于「现在健不健康」，
 *     已在状态页 ② 段。本页原来那个环让两页看起来像同一个页面。
 *   - 也不再复述命中率的百分点 → 结论段一句话带出，主体留给**曲线**。
 * 留下来的三段：
 *   ① 结论   —— 一句话账单（读了多少 / 省了多少 / 省掉多少次寻道）
 *   ② 账单   —— 累计四个数 + 最新 1s 采样对比条
 *   ③ 趋势   —— 命中率 / 读吞吐 / 写吞吐三条曲线（2 分钟滚动窗口）
 *   ④ 形状   —— 64 分片读热力（哪些分片在承压）
 *   ⑤ 口径   —— 采样与计数器纪律脚注
 *
 * 口径纪律：一切曲线来自 1s 真实采样的差值（stats-history.ts），
 * 无读流量时**每一个图位各自**显式空态——不画 0 曲线冒充收益
 * （对应 WPF 诚实降级约定）。空态改成逐图位渲染而非整页隐藏：
 * 改前 `!traffic` 直接把三段一起抹掉，读者看不出"这里本该有图"。
 */
export default function BenefitPage() {
  const statsQuery = useQuery<RbStats>({ queryKey: ['rb', 'stats'], enabled: false, retry: 0 });
  const stats: RbStats | undefined = statsQuery.data;
  const history = getStatsHistory();
  const traffic = hasReadTraffic();
  const writeTraffic = hasWriteTraffic();

  const hitRate =
    stats && stats.ReadBytes > 0 ? (stats.ReadHitBytes / stats.ReadBytes) * 100 : null;
  const readMB = stats ? stats.ReadBytes / 1048576 : 0;
  const savedMB = stats ? stats.ReadHitBytes / 1048576 : 0;
  const savedReqs = stats?.ReadHit ?? 0;

  const hitSeries = history.map((s) => [fmtTime(s.t), s.hitRate]);
  const rateSeries = history.map((s) => [fmtTime(s.t), s.hitMBps, s.diskMBps]);

  // 分片热力：取最近一个有流量的采样（StripeRead 累计差值 → 8×8 网格）
  const lastActive: StatsSample | undefined = [...history]
    .reverse()
    .find((s) => s.stripeRate.some((v) => v > 0) || s.stripeWriteRate.some((v) => v > 0));
  const heatData: Array<[number, number, number]> = [];
  if (lastActive) {
    lastActive.stripeRate.forEach((v, i) => {
      heatData.push([Math.floor(i / 8), i % 8, Number(v.toFixed(2))]);
    });
  }
  const heatMax = heatData.length ? Math.max(...heatData.map((d) => d[2])) : 1;

  const lastSample = history[history.length - 1];

  /* ① 结论：把四个数字压成一句人话。没有读流量时也要给结论（"还没有样本"），
   * 而不是留一句"暂无数据"让读者自己猜是不是坏了。 */
  const verdict =
    !stats
      ? '尚未取到统计快照——驱动可能未加载。到「加速」页确认通道状态。'
      : hitRate === null
        ? `本次开机已读 ${fmtMB(stats.ReadBytes)} MB，但还没有一笔读命中（缓存未预热或卷尚未接管）。曲线与热力会在命中出现后开始填充。`
        : `本次开机已读 ${fmtMB(stats.ReadBytes)} MB，其中 ${fmtMB(stats.ReadHitBytes)} MB 直接来自内存 —— 相当于替这块盘省掉 ${savedReqs.toLocaleString('zh-CN')} 次寻道。`;

  return (
    <div className="flex flex-col gap-6">
      <PageHeader
        title="缓存收益"
        desc="命中率 / 吞吐 / 分片热力——一切曲线来自 1s 真实采样差值，空态不画假图"
        actions={
          <Badge tone={traffic ? 'ok' : 'neutral'}>{traffic ? '采样中' : '尚无读流量'}</Badge>
        }
      />

      <p className="rbc-fade-up text-sm" style={{ color: 'var(--rbc-text-secondary)' }}>
        {verdict}
      </p>

      {/* ② 账单：累计四个数（这是本页的"成绩单"实体）+ 最新 1s 采样对比条 */}
      <section className="rbc-section rbc-fade-up">
        <div className="rbc-section-head">
          <span className="rbc-kicker">账单 · 本次开机累计</span>
          <span className="rbc-section-note">以字节省下的磁盘读，不是命中次数</span>
        </div>
        <div className="rbc-section-body">
          <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
            <Metric
              label="累计读流量"
              value={
                <span className="rbc-num">
                  {stats ? <AnimatedValue value={readMB} format={(n) => n.toFixed(1)} /> : '—'}
                </span>
              }
              hint="MB"
            />
            <Metric
              label="命中免下盘"
              value={
                <span className="rbc-num">
                  {stats ? <AnimatedValue value={savedMB} format={(n) => n.toFixed(1)} /> : '—'}
                </span>
              }
              hint="MB"
              tone={savedMB > 0 ? 'ok' : undefined}
            />
            <Metric
              label="字节命中率"
              value={hitRate === null ? '—' : `${hitRate.toFixed(1)}%`}
              hint={hitRate === null ? '尚无读流量' : undefined}
            />
            <Metric
              label="命中读次数"
              value={<span className="rbc-num">{savedReqs.toLocaleString('zh-CN')}</span>}
              hint="次"
            />
          </div>

          <div className="rbc-card">
            <div
              className="mb-2 flex items-baseline justify-between text-xs"
              style={{ color: 'var(--rbc-text-secondary)' }}
            >
              <span>最新 1s 采样对比</span>
              <span style={{ color: 'var(--rbc-text-tertiary)' }}>
                {lastSample ? fmtTime(lastSample.t) : '尚未采样'}
              </span>
            </div>
            <DualBarCompare
              hitMBps={lastSample?.hitMBps ?? 0}
              diskMBps={lastSample?.diskMBps ?? 0}
              writeMBps={lastSample?.writeMBps ?? 0}
            />
          </div>
        </div>
      </section>

      {/* ③ 趋势：三张曲线各自独立空态门——只写工作负载也该看到写曲线 */}
      <section className="rbc-section rbc-fade-up">
        <div className="rbc-section-head">
          <span className="rbc-kicker">趋势 · 2 分钟滚动窗口</span>
          <span className="rbc-section-note">无流量区间断线，不以 0 填充</span>
        </div>
        <div className="rbc-section-body">
          <Card
            title="字节命中率趋势（2 分钟窗口）"
            actions={
              <span className="rbc-num text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
                {hitRate === null ? '—' : `当前 ${hitRate.toFixed(1)}%`}
              </span>
            }
          >
            {traffic ? (
              <Chart
                label="字节命中率趋势折线图"
                height={200}
                option={{
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
                      /* 兜底值同步改：原来写 `#1f4cc2ff`，与令牌犯的是同一个错
                         （把 alpha 当首位），兜底一旦生效就是一块不透明深蓝。 */
                      areaStyle: { color: token('--rbc-chart-area-fill', 'rgba(15,134,224,0.12)') },
                    },
                  ],
                }}
              />
            ) : (
              <ChartEmpty>
                尚无读流量：命中率曲线会在挂载卷并产生读 I/O 后出现。
                空窗口不画 0 曲线冒充收益。
              </ChartEmpty>
            )}
          </Card>

          <Card title="读吞吐对比：命中 vs 下盘（MB/s）">
            {traffic ? (
              <Chart
                label="命中读与下盘读吞吐对比折线图"
                height={220}
                option={{
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
            ) : (
              <ChartEmpty>
                尚无读流量：两条曲线分别是「命中就直接返回的字节速率」与「仍然不得不下盘的速率」，
                二者的面积比就是命中率曲线的另一种读法。
              </ChartEmpty>
            )}
          </Card>

          <Card title="写吞吐（MB/s）：总写 vs 进缓存">
            {writeTraffic ? (
              <Chart
                label="写吞吐折线图：总写与进缓存"
                height={180}
                option={{
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
              <ChartEmpty>
                尚无写流量。直写模式下「进缓存」恒为 0 是诚实值（写直落盘），
                回写模式写缓存标脏即计。
              </ChartEmpty>
            )}
          </Card>
        </div>
      </section>

      {/* ④ 形状：热力说明"活是什么形状的"——是否偏斜、有没有冷分片 */}
      <section className="rbc-section rbc-fade-up">
        <div className="rbc-section-head">
          <span className="rbc-kicker">形状 · 分片读热力</span>
          <span className="rbc-section-note">64 分片 × 最近活跃采样，颜色 = 该分片的读速率</span>
        </div>
        <div className="rbc-section-body">
          <Card title="读速率分布（MB/s）">
            {lastActive ? (
              <Chart
                label="64 分片读速率热力图，8×8 网格"
                height={200}
                option={{
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
            ) : (
              <ChartEmpty>
                还没有分片活跃采样。热力取「最近一次有流量的 1s 采样」——
                全零时不画一张全冷的假矩阵，以免被读成"缓存分配严重偏斜"。
              </ChartEmpty>
            )}
          </Card>
          <p className="rbc-section-note">
            冷热只反映<b style={{ color: 'var(--rbc-text-secondary)' }}>读</b>速率；写速率与累计计数见状态页
            分片矩阵（那里用颜色 + 高度双通道编码，色盲可辨）。
          </p>
        </div>
      </section>

      <p className="rbc-section-note">
        口径：字节命中率以字节为准（与驱动统计一致）；吞吐/热力为 1s 采样差值，
        计数器回退（驱动重启）记 0 不记负；延迟明细见状态页。缓存占用率与生效上限同样在状态页，
        本页只谈收益与趋势，避免两页重复。
      </p>
    </div>
  );
}

/** 双条对比：最新采样 命中/下盘/总写 三条水平条（WPF DualBarCompare 语义） */
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
            <span className="rbc-num text-xs" style={{ color: 'var(--rbc-text-primary)' }}>
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
