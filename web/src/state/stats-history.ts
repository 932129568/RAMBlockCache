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
      const delta = (cur: number, old: number): number =>
        cur >= old ? (cur - old) / dt / MB : 0; // 计数器回退=驱动重启，记 0 不记负
      sample.hitMBps = delta(stats.ReadHitBytes, prev.stats.ReadHitBytes);
      sample.diskMBps = delta(
        stats.ReadBytes - stats.ReadHitBytes,
        prev.stats.ReadBytes - prev.stats.ReadHitBytes,
      );
      sample.writeMBps = delta(stats.WriteBytes, prev.stats.WriteBytes);
      sample.cachedWriteMBps = delta(stats.WriteCached, prev.stats.WriteCached);
      for (let i = 0; i < 64; i++) {
        // noUncheckedIndexedAccess：数组索引访问按可能缺失处理，缺位记 0
        sample.stripeRate[i] = delta(stats.StripeRead[i] ?? 0, prev.stats.StripeRead[i] ?? 0);
        sample.stripeWriteRate[i] = delta(
          stats.StripeWrite[i] ?? 0,
          prev.stats.StripeWrite[i] ?? 0,
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
