/**
 * 统计采样历史 —— 收益页的序列数据源。
 *
 * 采样由 App 级唯一轮询器（StatsPoller，1s）推入；本模块只做差值换算与环形存储。
 * 差值口径与 WPF MainViewModel.SampleStripeHeat 一致：累计计数器取相邻采样差，
 * 计数器回退（驱动重启）视为清零重建，该周期差值记 0，绝不画负值/假值。
 */

import type { RbStats } from '@protocol/types';

export interface StatsSample {
  /** 采样时刻（本地毫秒） */
  t: number;
  /** 字节命中率 %；无读流量 = null（不画假值） */
  hitRate: number | null;
  /** 命中读 MB/s（本采样周期差值） */
  hitMBps: number;
  /** 下盘读 MB/s（ReadBytes - ReadHitBytes 的周期差值） */
  diskMBps: number;
  /** 命中读延迟 EWMA µs（0=未采样，透传） */
  cacheLatUs: number;
  /** 下盘读延迟 EWMA µs（0=未采样，透传） */
  diskLatUs: number;
  /** 64 分片读速率 MB/s（StripeRead 差值；热力图消费） */
  stripeRate: number[];
  /** 总写吞吐 MB/s（WriteBytes 差值；直写模式下≈磁盘写，回写=进缓存速度） */
  writeMBps: number;
  /** 进缓存写 MB/s（WriteCached 差值；直写模式=0 是诚实值不是缺陷） */
  cachedWriteMBps: number;
  /** 64 分片写速率 MB/s（StripeWrite 差值；WPF StripeHeat 写侧对称面） */
  stripeWriteRate: number[];
}

const RING_MAX = 120; // 1s × 120 = 2 分钟窗口
const ring: StatsSample[] = [];
let prev: { t: number; stats: RbStats } | null = null;

const MB = 1024 * 1024;

export function recordStatsSample(stats: RbStats): void {
  const now = Date.now();
  const sample: StatsSample = {
    t: now,
    hitRate: stats.ReadBytes > 0 ? (stats.ReadHitBytes / stats.ReadBytes) * 100 : null,
    hitMBps: 0,
    diskMBps: 0,
    cacheLatUs: stats.CacheReadLatencyUs,
    diskLatUs: stats.DiskReadLatencyUs,
    stripeRate: new Array<number>(64).fill(0),
    writeMBps: 0,
    cachedWriteMBps: 0,
    stripeWriteRate: new Array<number>(64).fill(0),
  };

  if (prev) {
    const dt = (now - prev.t) / 1000;
    if (dt > 0.05) {
      /* [FIX 2026-11-13] 回退检测必须在**原始计数器**上做，不能对派生差值做。
       * 旧实现用 `ReadBytes - ReadHitBytes` 的差值比较：驱动重启后
       * ReadBytes 归零再涨、ReadHitBytes 归零，两个派生差值可能满足
       * `cur >= old`，于是一次重启被画成一个正向吞吐尖峰
       * （例：prev 10,000,000/5,000,000 → 重启后 10,000,001/1，
       *  派生差值 5,000,000 → 10,000,000，被当成 +4.77 MB/s 的真实流量）。
       * 现改为：任一原始计数器回退 ⇒ 本次采样整体视为重建，全部记 0。 */
      const diff = (cur: number, old: number): number | null =>
        cur >= old ? cur - old : null; // null = 该计数器回退（驱动重启）
      const rate = (bytes: number | null): number => (bytes === null ? 0 : bytes / dt / MB);

      const dRead = diff(stats.ReadBytes, prev.stats.ReadBytes);
      const dHit = diff(stats.ReadHitBytes, prev.stats.ReadHitBytes);
      // 两个原始量都有效才计算「下盘读」；dHit > dRead 属不可能的计数组合，按 0 处理
      const dDisk =
        dRead === null || dHit === null ? null : Math.max(0, dRead - dHit);

      sample.hitMBps = rate(dHit);
      sample.diskMBps = rate(dDisk);
      sample.writeMBps = rate(diff(stats.WriteBytes, prev.stats.WriteBytes));
      sample.cachedWriteMBps = rate(diff(stats.WriteCached, prev.stats.WriteCached));
      for (let i = 0; i < 64; i++) {
        // noUncheckedIndexedAccess：数组索引访问按可能缺失处理，缺位记 0
        sample.stripeRate[i] = rate(diff(stats.StripeRead[i] ?? 0, prev.stats.StripeRead[i] ?? 0));
        sample.stripeWriteRate[i] = rate(
          diff(stats.StripeWrite[i] ?? 0, prev.stats.StripeWrite[i] ?? 0),
        );
      }
    }
  }
  prev = { t: now, stats };

  ring.push(sample);
  if (ring.length > RING_MAX) ring.shift();
}

/** 只读快照：调用方拿到副本，渲染期遍历安全 */
export function getStatsHistory(): StatsSample[] {
  return ring.slice();
}

/** 是否出现过真实读流量（决定收益页画图还是显式空态） */
export function hasReadTraffic(): boolean {
  return ring.some((s) => s.hitRate !== null || s.hitMBps > 0 || s.diskMBps > 0);
}

/** 是否出现过真实写流量（写吞吐图空态判定） */
export function hasWriteTraffic(): boolean {
  return ring.some((s) => s.writeMBps > 0 || s.cachedWriteMBps > 0);
}
