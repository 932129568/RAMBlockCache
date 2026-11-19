/* 配置回读 diff —— 「请求值 vs 驱动实际生效值」
 *
 * 为什么必须做这件事（[R19 2026-11-13]）：
 *   SET_CONFIG 返回 Status=0 **只代表驱动接受了这次写入**，不代表每个字段都按请求生效。
 *   驱动侧对多处字段做 clamp / 归一化，且都是**静默**的：
 *     - `rbL2.c:642`  L2 容量 > 128GB → 截到 128（Web 曾放行 512，见 221 F4）
 *     - `BlockSize`   白名单 4K/8K/16K/32K/64K，其它值回落到默认（11-03 事故封堵）
 *     - `DirtyLowPct` 与 `DirtyHighPct` 的先后顺序会被纠正
 *     - `CacheSizeMB` 在 `AutoMemory=1` 时只是上限兜底，实际容量另算
 *   于是「表单显示 512GB、驱动实际 128GB」这类界面谎言在旧实现里完全不可见。
 *   CLI (`ramboostctl.c:273 FIX[R5]`) 早就有回读，Web 侧缺失 —— 本文件补上。
 *
 * 设计纪律：
 *   - **纯函数**，不碰 transport / React，便于单测（见 config-diff.test.ts）。
 *   - 比较**整个 payload** 而不是「表单注册过的字段」：未放开的字段是以驱动原文为基底
 *     带过去的，若驱动又改了它，同样属于「用户看到一套、驱动生效另一套」，必须暴露。
 *   - 空数组 = 完全一致（这才允许 UI 说「已按请求生效」）。
 */

import type { RbConfig } from '../../protocol/rb-config.generated';

/** 单个字段的请求值 / 生效值差异。 */
export interface ConfigDiff {
  field: keyof RbConfig & string;
  /** 表单提交的值（已在 RB_CONFIG 结构内） */
  want: number | string;
  /** 回读得到的驱动实际值 */
  got: number | string;
}

/** 中文短名：让提示直接可读，不必让用户去对 PascalCase 字段名。 */
const LABELS: Partial<Record<string, string>> = {
  Enabled: '总开关',
  ReadCacheEnabled: '读缓存',
  WriteCacheEnabled: '写缓存',
  WriteBack: '全局写模式',
  CacheSizeMB: '缓存上限(MB)',
  FlushIntervalMs: '回写间隔(ms)',
  DirtyHighPct: '脏页高水位(%)',
  DirtyLowPct: '脏页低水位(%)',
  SystemVolumeCached: '系统卷缓存',
  IdleReclaimEnabled: '空闲回收',
  IdleReclaimSec: '空闲回收阈值(s)',
  ParallelFlush: '并行回写路数',
  CachePagingIo: '分页 IO 入缓存',
  WriteRelease: '写释放',
  Frozen: '冻结',
  IdleFlush: '空闲刷写',
  VolumeFilter: '卷过滤表',
  ReadaheadEnabled: '预取引擎',
  ReadaheadWindowKB: '预取窗口(KB)',
  AutoMemory: '自动内存预算',
  MemReserveMB: '内存预留(MB)',
  AutoUsePct: '自动占用上限(%)',
  BlockSize: '块大小',
  TinyLFU: 'TinyLFU 准入',
};

export function configFieldLabel(field: string): string {
  return LABELS[field] ?? field;
}

function normalize(v: unknown): string {
  // VolumeFilter 是 wchar_t[8][4]，桥接可能给数组也可能给定长字符串；统一成无尾零的形式。
  if (Array.isArray(v)) return v.map((s) => String(s).replace(/\0+$/, '')).join('\u0000');
  if (typeof v === 'string') return v.replace(/\0+$/, '');
  if (typeof v === 'number') return Number.isFinite(v) ? String(v) : 'NaN';
  return v === undefined || v === null ? '' : String(v);
}

/**
 * 比较「请求的配置」与「回读到的配置」，返回逐字段差异（顺序 = 对象键序）。
 *
 * 只比较 `want` 里存在的键：调用方以驱动原文为基底合并，因此 want 覆盖全部字段。
 */
export function diffConfig(want: Partial<RbConfig>, got: Partial<RbConfig>): ConfigDiff[] {
  const out: ConfigDiff[] = [];
  for (const key of Object.keys(want) as Array<keyof RbConfig & string>) {
    const w = want[key];
    const g = got[key];
    if (normalize(w) === normalize(g)) continue;
    out.push({ field: key, want: w as number | string, got: g as number | string });
  }
  return out;
}

/** 把差异渲染成人类可读的一段文本（多行，供结果面板直接展示）。 */
export function describeDiff(diffs: ReadonlyArray<ConfigDiff>): string {
  return diffs
    .map((d) => `${configFieldLabel(d.field)}：请求 ${normalize(d.want) || '(空)'} → 生效 ${normalize(d.got) || '(空)'}`)
    .join('\n');
}
