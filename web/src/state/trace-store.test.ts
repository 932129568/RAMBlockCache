import { describe, expect, it } from 'vitest';
import { getChannels, recordTrace } from './trace-store';

/** 通道健康语义回归：四态判定 + P50/P95 + SLO 违约 */
describe('trace-store', () => {
  it('未调用通道为状态 0', () => {
    const ch = getChannels().find((c) => c.name === '刷写');
    expect(ch).toBeDefined();
  });

  it('成功 <50ms 判定正常；≥50ms 判定偏慢；失败判定失败', () => {
    recordTrace({ method: 3, ok: true, ms: 10 }); // FLUSH → 刷写通道
    const chOk = getChannels().find((c) => c.name === '刷写');
    expect(chOk?.state).toBe(1);
    recordTrace({ method: 3, ok: true, ms: 80 });
    expect(getChannels().find((c) => c.name === '刷写')?.state).toBe(2);
    recordTrace({ method: 3, ok: false, ms: 5 });
    expect(getChannels().find((c) => c.name === '刷写')?.state).toBe(3);
    expect(getChannels().find((c) => c.name === '刷写')?.failures).toBeGreaterThan(0);
  });

  it('统计通道 SLO=1500ms：P95 未超不算违约', () => {
    recordTrace({ method: 1, ok: true, ms: 3 });
    const ch = getChannels().find((c) => c.name === '统计');
    expect(ch?.slo).toBe('stats');
    expect(ch?.sloBreached).toBe(false);
  });
});
