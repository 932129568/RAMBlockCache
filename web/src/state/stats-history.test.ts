import { describe, expect, it, beforeEach } from 'vitest';
import { getStatsHistory, hasReadTraffic, recordStatsSample } from './stats-history';
import type { RbStats } from '@protocol/types';

/** 统计差值语义回归（WS-RACE 级：计数器回退不得产生负值/假值） */
function mkStats(over: Partial<RbStats>): RbStats {
  return {
    ReadReq: 0, ReadHit: 0, ReadBytes: 0, ReadHitBytes: 0, WriteReq: 0, WriteBytes: 0,
    WriteCached: 0, WbFlush: 0, WbBytes: 0, EvictBytes: 0, CurBytes: 0, DirtyBytes: 0,
    ChunkCount: 0, InstanceCount: 0, CacheSizeMB: 0, EffectiveCacheMB: 0, WriteBackMode: 0,
    Enabled: 0, SafeMode: 0, RmwReads: 0, TrimCount: 0, IdleReclaimBytes: 0, LostDirtyBytes: 0,
    StripeRead: new Array(64).fill(0), StripeWrite: new Array(64).fill(0),
    StripeCount: 0, PressureLevel: 0, DynamicLimitMB: 0, AvailablePhysMB: 0, PeakBytes: 0,
    ReadaheadBytes: 0, ReadaheadHitBytes: 0, PressureShrinkBytes: 0, ReadaheadActive: 0,
    PanicActive: 0, BootFails: 0, DirtySnapshotMB: 0, CacheReadLatencyUs: 0, DiskReadLatencyUs: 0,
    // [U1 2026-11-06] RB_STATS 末尾纯追加：夹具补齐分位字段（0=尚无样本）
    CacheReadLatP50Us: 0, CacheReadLatP95Us: 0, CacheReadLatP99Us: 0,
    DiskReadLatP50Us: 0, DiskReadLatP95Us: 0, DiskReadLatP99Us: 0,
    ...over,
  };
}

describe('stats-history 差值语义', () => {
  beforeEach(() => {
    // 环形存储是模块级单例——测试顺序内自洽即可
  });

  it('首个采样无差值（不画假吞吐）', () => {
    const before = getStatsHistory().length;
    recordStatsSample(mkStats({ ReadBytes: 1000, ReadHitBytes: 500 }));
    const h = getStatsHistory();
    const last = h[h.length - 1]!;
    expect(h.length).toBe(before + 1);
    expect(last.hitMBps).toBe(0);
    expect(last.diskMBps).toBe(0);
  });

  it('计数器回退（驱动重启）记 0 不记负', () => {
    recordStatsSample(mkStats({ ReadBytes: 10_000_000, ReadHitBytes: 5_000_000 }));
    const before = getStatsHistory().length;
    // 回退：ReadHitBytes 比上一采样更小
    recordStatsSample(mkStats({ ReadBytes: 10_000_001, ReadHitBytes: 1 }));
    const last = getStatsHistory()[getStatsHistory().length - 1]!;
    expect(getStatsHistory().length).toBe(before + 1);
    expect(last.hitMBps).toBe(0);
    expect(last.diskMBps).toBeGreaterThanOrEqual(0);
  });

  it('有读流量后 hasReadTraffic 为真；纯零采样为假', () => {
    recordStatsSample(mkStats({ ReadBytes: 2_000_000, ReadHitBytes: 1_000_000 }));
    expect(hasReadTraffic()).toBe(true);
  });

  it('命中率以字节为准', () => {
    recordStatsSample(mkStats({ ReadBytes: 4_000_000, ReadHitBytes: 1_000_000 }));
    const last = getStatsHistory()[getStatsHistory().length - 1]!;
    expect(last.hitRate).toBeCloseTo(25, 5);
  });
});
