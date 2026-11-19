import { describe, expect, it } from 'vitest';
import { configFieldLabel, describeDiff, diffConfig } from './config-diff';
import type { RbConfig } from '../../protocol/rb-config.generated';

/** 构造一份"驱动原文"，只关心参与比较的字段。 */
function cfg(over: Partial<RbConfig> = {}): RbConfig {
  return {
    Enabled: 1,
    ReadCacheEnabled: 1,
    WriteCacheEnabled: 1,
    WriteBack: 0,
    CacheSizeMB: 0,
    FlushIntervalMs: 1000,
    DirtyHighPct: 70,
    DirtyLowPct: 50,
    SystemVolumeCached: 0,
    IdleReclaimEnabled: 1,
    IdleReclaimSec: 30,
    ParallelFlush: 4,
    CachePagingIo: 0,
    WriteRelease: 0,
    Frozen: 0,
    IdleFlush: 0,
    VolumeFilter: ['', '', '', '', '', '', '', ''],
    ReadaheadEnabled: 1,
    ReadaheadWindowKB: 256,
    AutoMemory: 1,
    MemReserveMB: 1024,
    AutoUsePct: 50,
    BlockSize: 65536,
    TinyLFU: 1,
    ...over,
  };
}

/** 配置回读 diff 回归：这是「界面不再替驱动担保」的机器判据。 */
describe('config-diff', () => {
  it('完全一致时返回空数组（才允许 UI 宣称"回读核对通过"）', () => {
    expect(diffConfig(cfg(), cfg())).toEqual([]);
  });

  it('驱动静默 clamp 的字段必须被点名（L2/块大小类）', () => {
    const want = cfg({ CacheSizeMB: 524288 }); // 请求 512GB
    const got = cfg({ CacheSizeMB: 131072 }); // 驱动截到 128GB
    const diffs = diffConfig(want, got);
    expect(diffs).toHaveLength(1);
    expect(diffs[0]?.field).toBe('CacheSizeMB');
    expect(describeDiff(diffs)).toContain('请求 524288 → 生效 131072');
  });

  it('多个字段同时漂移时逐项列出，顺序稳定', () => {
    const want = cfg({ BlockSize: 131072, DirtyLowPct: 60 });
    const got = cfg({ BlockSize: 65536, DirtyLowPct: 50 });
    const diffs = diffConfig(want, got);
    expect(diffs.map((d) => d.field)).toEqual(['DirtyLowPct', 'BlockSize']);
    expect(describeDiff(diffs).split('\n')).toHaveLength(2);
  });

  it('undefined 与 0 不等价：回读缺字段不得被当成"生效为 0"而静默通过', () => {
    const want = cfg();
    const got = { ...cfg() } as Partial<RbConfig>;
    delete got.TinyLFU;
    const diffs = diffConfig(want, got);
    expect(diffs.map((d) => d.field)).toEqual(['TinyLFU']);
  });

  it('VolumeFilter 忽略尾部 NUL：驱动截断补零不算漂移', () => {
    const want = cfg({ VolumeFilter: ['C', '', '', '', '', '', '', ''] });
    const got = cfg({ VolumeFilter: ['C\0\0\0', '', '', '', '', '', '', ''] });
    expect(diffConfig(want, got)).toEqual([]);
  });

  it('字段中文短名覆盖高频项，未知字段回落原字段名', () => {
    expect(configFieldLabel('BlockSize')).toBe('块大小');
    expect(configFieldLabel('NotARealField')).toBe('NotARealField');
  });
});
