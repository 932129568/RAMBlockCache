import { useEffect, useState } from 'react';
import { motion } from 'motion/react';
import {
  useChannels,
  type ChannelState,
  type ChannelView,
} from '@state/trace-store';
import { DUR, EASE, useMotion } from '@lib/motion';

/**
 * IOCTL 通道健康点阵 —— WPF Controls/IoctlMatrix.cs 的 web 移植。
 *
 * 布局算法逐条对照 CalculateLayout()：实际文字宽度决定最小格宽（minCell≥96），
 * 窄栏优先减列（1..min(6,count)），行高固定 42px 不缩，格内逐格裁剪（overflow hidden）。
 * 四态：正常=success / 偏慢(≥50ms)=warning / 失败=danger / 未调用=neutral+"—"。
 * 数据来自 trace-store（经 driver-store 注入的 onTrace 回调埋点），纯展示零轮询。
 */

const SLO_LIMIT: Record<string, number | null> = {
  command: 200,
  stats: 1500,
  events: 600,
};

/** 与 IoctlMatrix.cs 同款测宽：canvas measureText 替代 FormattedText */
function measureText(text: string): number {
  const ctx = measureCtx();
  return ctx ? ctx.measureText(text).width : text.length * 7;
}

let _ctx: CanvasRenderingContext2D | null | undefined;
function measureCtx(): CanvasRenderingContext2D | null | undefined {
  if (_ctx !== undefined) return _ctx;
  try {
    const c = document.createElement('canvas');
    _ctx = c.getContext('2d');
    if (_ctx) _ctx.font = '11px "Segoe UI", "Microsoft YaHei UI", sans-serif';
  } catch {
    _ctx = null;
  }
  return _ctx;
}

function layout(availableWidth: number, items: ChannelView[]) {
  const count = items.length;
  if (count === 0) return { columns: 1, cellWidth: 96 };
  let longest = 0;
  for (const it of items) {
    longest = Math.max(longest, measureText(it.name), measureText(msText(it)));
  }
  const minCell = Math.max(96, Math.ceil(30 + longest + 8));
  const maxColumns = Math.min(6, count);
  const columns =
    !Number.isFinite(availableWidth) || availableWidth <= 0
      ? maxColumns
      : Math.max(1, Math.min(maxColumns, Math.floor(availableWidth / minCell)));
  return { columns, cellWidth: availableWidth / columns };
}

function msText(it: ChannelView): string {
  if (it.lastMs === null) return '—';
  const s = it.lastMs < 1 ? '<1' : String(Math.round(it.lastMs * 100) / 100);
  return it.state === 0 ? s : `${s} ms`;
}

const STATE_STYLE: Record<ChannelState, { dot: string; label: string }> = {
  0: { dot: 'var(--rbc-neutral-fg)', label: '未调用' },
  1: { dot: 'var(--rbc-success-fg)', label: '正常' },
  2: { dot: 'var(--rbc-warning-fg)', label: '偏慢' },
  3: { dot: 'var(--rbc-danger-fg)', label: '失败' },
};

/**
 * [UI-2026-11-12] `items` 改为可选受控 prop。
 *
 * 改前组件内部自己 `useState + setInterval(500)` 轮询 traceVersion()。
 * 诊断页需要在**结论段**里说出"12 个通道里几个超了 SLO"，如果让它再写一份
 * 同样的轮询，同一页面就有两条 500ms 定时器，各自 `getChannels()` 重算一遍
 * 12×32 样本的分位数。现在页面持有唯一订阅（`useChannels()`）并把快照传进来；
 * 不传时组件自己订阅，单独使用时行为不变。
 */
export default function IoctlMatrix({ items: controlled }: { items?: ChannelView[] } = {}) {
  const { reduce } = useMotion();
  const subscribed = useChannels(500, controlled === undefined);
  const items = controlled ?? subscribed;
  const [width, setWidth] = useState(0);
  const [ref, setRef] = useState<HTMLDivElement | null>(null);

  useEffect(() => {
    if (!ref) return;
    const ro = new ResizeObserver((entries) => {
      setWidth(entries[0]?.contentRect.width ?? 0);
    });
    ro.observe(ref);
    return () => ro.disconnect();
  }, [ref]);

  const { columns } = layout(width || 0, items);
  const rows = Math.ceil(items.length / columns);

  return (
    <div ref={setRef} role="img" aria-label="IOCTL 通道健康点阵，12 通道四态">
      <div
        className="grid"
        style={{ gridTemplateColumns: `repeat(${columns}, minmax(0, 1fr))`, gap: 2 }}
      >
        {items.map((it, i) => {
          const st = STATE_STYLE[it.state];
          const limit = it.slo ? SLO_LIMIT[it.slo] : null;
          return (
            <motion.div
              key={it.name}
              /* [A7 2026-11-11] 三处动效：
                 1. 入场 stagger（点阵按网格顺序浮现，比整块闪现更有"仪表点亮"感）；
                 2. 状态点颜色变化补过渡 —— 通道从"正常"掉到"偏慢/失败"时，
                    颜色硬跳会让用户错过这次劣化；现在有 180ms 渐变可被感知；
                 3. 延迟迷你条宽度用 transition（原本是硬跳，每秒轮询都闪一下）。 */
              initial={reduce ? false : { opacity: 0, scale: 0.96 }}
              animate={{ opacity: 1, scale: 1 }}
              transition={{
                duration: reduce ? 0 : DUR.base,
                ease: EASE.decel,
                delay: reduce ? 0 : Math.min(i * 0.012, 0.2),
              }}
              className={['rbc-iom-cell', it.sloBreached ? 'rbc-breach' : ''].filter(Boolean).join(' ')}
              title={`${st.label}${it.lastMs !== null ? ` · ${it.lastMs}ms` : ''}${
                it.p95 !== null ? ` · P95 ${it.p95}ms` : ''
              }${limit !== null ? ` · SLO ${limit}ms` : ''}${
                it.failures > 0 ? ` · 失败 ${it.failures} 次` : ''
              }`}
              style={{
                height: 42,
                overflow: 'hidden',
                padding: '3px 0 0 8px',
                background: 'var(--rbc-bg-elevated)',
                border: `1px solid ${it.sloBreached ? 'var(--rbc-warning-border)' : 'var(--rbc-border)'}`,
                borderRadius: 'var(--rbc-radius-sm)',
              }}
            >
              <div style={{ display: 'flex', alignItems: 'center', gap: 5 }}>
                <span
                  aria-hidden
                  style={{
                    width: 10,
                    height: 10,
                    borderRadius: '50%',
                    background: st.dot,
                    flexShrink: 0,
                    transition: 'background-color var(--rbc-dur-180) ease',
                  }}
                />
                <span
                  className="text-[11px]"
                  style={{
                    color: 'var(--rbc-text-secondary)',
                    overflow: 'hidden',
                    textOverflow: 'ellipsis',
                    whiteSpace: 'nowrap',
                  }}
                >
                  {it.name}
                </span>
                {it.sloBreached && (
                  <span className="text-[10px]" style={{ color: 'var(--rbc-warning-fg)' }}>
                    SLO!
                  </span>
                )}
              </div>
              <div
                className="font-mono text-[11px]"
                style={{
                  color: it.state === 0 ? 'var(--rbc-text-tertiary)' : 'var(--rbc-text-secondary)',
                  paddingLeft: 15,
                  overflow: 'hidden',
                  textOverflow: 'ellipsis',
                  whiteSpace: 'nowrap',
                }}
              >
                {msText(it)}
              </div>
              {/* 延迟迷你条（满刻度 200ms，与命令 SLO 一致；未调用不画） */}
              <div
                style={{
                  marginLeft: 15,
                  marginTop: 2,
                  height: 4,
                  width: 'calc(100% - 24px)',
                  background: 'var(--rbc-chart-track)',
                  borderRadius: 2,
                }}
              >
                {it.lastMs !== null && it.state !== 0 && (
                  <div
                    style={{
                      height: 4,
                      width: `${Math.max(2, Math.min(100, (it.lastMs / 200) * 100))}%`,
                      background: st.dot,
                      borderRadius: 2,
                      /* 宽度过渡与 StatsPoller 的 1s 节奏匹配：320ms 足够看清
                         "条子长了一截"，又不会拖到下一帧数据到来。 */
                      transition:
                        'width var(--rbc-dur-320) ease, background-color var(--rbc-dur-180) ease',
                    }}
                  />
                )}
              </div>
            </motion.div>
          );
        })}
      </div>
      <div className="mt-2 flex gap-4 text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
        {([1, 2, 3, 0] as ChannelState[]).map((s) => (
          <span key={s} style={{ display: 'inline-flex', alignItems: 'center', gap: 4 }}>
            <span
              aria-hidden
              style={{ width: 8, height: 8, borderRadius: '50%', background: STATE_STYLE[s].dot }}
            />
            {STATE_STYLE[s].label}
          </span>
        ))}
        <span className="flex-1" />
        <span>{rows} 行 × {columns} 列 · SLO：命令≤200ms 统计≤1.5s 事件≤600ms</span>
      </div>
    </div>
  );
}
