/**
 * IOCTL 通道健康追踪（对应 WPF IoctlTrace / A5 埋点的 web 侧实现）。
 *
 * 数据来源：driver-store 在构造 HttpTransport 时注入 onTrace 回调
 * （依赖方向：state → bridge 单向；bridge 只持有回调，不知道本文件存在）。
 *
 * 12 通道 × 四态（与 WPF IoctlMatrix 同口径）：
 *   0=未调用 1=正常(<50ms) 2=偏慢(≥50ms) 3=失败
 * 另维护每通道最近 32 次时延样本，供 P50/P95 与 SLO 判定（P1-7：
 * 命令 ≤200ms / 统计 ≤1.5s / 事件 ≤600ms）。
 */

import { useRef, useState } from 'react';
import { useVisibilityAwarePoll } from '../hooks/useVisibilityAwarePoll';

export interface TraceEvent {
  /** RB_MSG 消息号 */
  method: number;
  ok: boolean;
  /** 端到端耗时 ms */
  ms: number;
}

export type ChannelState = 0 | 1 | 2 | 3;

export interface ChannelView {
  method: number;
  name: string;
  state: ChannelState;
  /** 最近一次时延 ms（未调用=null） */
  lastMs: number | null;
  calls: number;
  failures: number;
  /** P50/P95（最近 32 样本，成功调用；样本<2 时 null） */
  p50: number | null;
  p95: number | null;
  /** P1-7 SLO 等级：'command'≤200ms / 'stats'≤1500ms / 'events'≤600ms / null=不判 */
  slo: 'command' | 'stats' | 'events' | null;
  sloBreached: boolean;
}

/** 消息号 → 通道名（未知消息并入 Config 通道不丢账，与 WPF MapChannel 同策略） */
const CHANNELS: Array<{ name: string; methods: number[]; slo: ChannelView['slo'] }> = [
  { name: '握手', methods: [10], slo: 'command' },
  { name: '统计', methods: [1], slo: 'stats' },
  { name: '配置', methods: [2, 4, 13], slo: 'command' },
  { name: '刷写', methods: [3], slo: 'command' },
  { name: '卷策略', methods: [6], slo: 'command' },
  { name: '卷枚举', methods: [7], slo: 'command' },
  { name: '挂载', methods: [8], slo: 'command' },
  { name: '日志', methods: [9], slo: 'command' },
  { name: '熔断', methods: [11], slo: 'command' },
  { name: '解除', methods: [12], slo: 'command' },
  { name: '安全模式', methods: [5], slo: 'command' },
  { name: '事件', methods: [14], slo: 'events' },
];

const RING = 32;

interface ChannelInternal {
  methods: Set<number>;
  name: string;
  slo: ChannelView['slo'];
  state: ChannelState;
  lastMs: number | null;
  calls: number;
  failures: number;
  samples: number[];
}

const byMethod = new Map<number, ChannelInternal>();
for (const ch of CHANNELS) {
  const internal: ChannelInternal = {
    methods: new Set(ch.methods),
    name: ch.name,
    slo: ch.slo,
    state: 0,
    lastMs: null,
    calls: 0,
    failures: 0,
    samples: [],
  };
  for (const m of ch.methods) byMethod.set(m, internal);
}

function percentile(sorted: number[], p: number): number | null {
  if (sorted.length < 2) return null;
  const idx = Math.max(0, Math.min(sorted.length - 1, Math.ceil((p / 100) * sorted.length) - 1));
  return sorted[idx] ?? null;
}

function sloLimit(slo: ChannelView['slo']): number | null {
  switch (slo) {
    case 'command':
      return 200;
    case 'stats':
      return 1500;
    case 'events':
      return 600;
    default:
      return null;
  }
}

export function recordTrace(e: TraceEvent): void {
  const ch = byMethod.get(e.method);
  if (!ch) return;
  ch.calls += 1;
  ch.lastMs = Math.round(e.ms * 10) / 10;
  if (!e.ok) {
    ch.failures += 1;
    ch.state = 3;
    return;
  }
  ch.samples.push(ch.lastMs);
  if (ch.samples.length > RING) ch.samples.shift();
  ch.state = ch.lastMs >= 50 ? 2 : 1;
}

export function getChannels(): ChannelView[] {
  return CHANNELS.map((def) => {
    // CHANNELS 每项 methods 非空（构造时已保证），noUncheckedIndexedAccess 需显式收窄
    const ch = byMethod.get(def.methods[0] as number)!;
    const sorted = [...ch.samples].sort((a, b) => a - b);
    const limit = sloLimit(ch.slo);
    const p95 = percentile(sorted, 95);
    return {
      method: def.methods[0] as number,
      name: ch.name,
      state: ch.state,
      lastMs: ch.lastMs,
      calls: ch.calls,
      failures: ch.failures,
      p50: percentile(sorted, 50),
      p95,
      slo: ch.slo,
      sloBreached: limit !== null && p95 !== null && p95 > limit,
    };
  });
}

/** 通道快照版本号：每次 record 自增，供 UI 订阅比对避免整表重渲染 */
let version = 0;
export function traceVersion(): number {
  return version;
}
export function bumpTraceVersion(): void {
  version += 1;
}

/**
 * [UI-2026-11-12] 通道快照的 React 订阅。
 *
 * 改前是 IoctlMatrix 自己在组件里 `useState + setInterval(500)` 轮询
 * `traceVersion()`。诊断页现在需要在**结论段**里说出"12 个通道里几个超 SLO"，
 * 若让它再写一份同样的轮询，同一个页面就会有两条 500ms 定时器各自
 * `getChannels()`（每次都要重算 12×32 样本的分位数）。
 *
 * 抽成 hook 后由页面持有唯一订阅，矩阵作为受控组件接收数据。
 * 只在 `traceVersion()` 变化时 setState —— 与原来的比较逻辑一致，
 * 不会因为"页面别处重渲染"而白算一遍。
 */
export function useChannels(pollMs = 500, enabled = true): ChannelView[] {
  const [items, setItems] = useState<ChannelView[]>(() => getChannels());
  const last = useRef(traceVersion());
  /* enabled=false 用于"父级已持有订阅"的受控场景：此时不能再开一条定时器，
   * 否则同一份数据被两个 500ms 轮询各算一遍（12×32 样本的分位数并不便宜）。
   * [237] 可见性感知：页面隐藏时整个定时器撤掉，恢复可见立即刷新一次。 */
  useVisibilityAwarePoll(
    () => {
      const v = traceVersion();
      if (v !== last.current) {
        last.current = v;
        setItems(getChannels());
      }
    },
    { intervalMs: pollMs, enabled },
  );
  return items;
}
