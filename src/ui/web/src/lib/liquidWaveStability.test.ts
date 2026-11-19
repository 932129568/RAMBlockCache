// src/ui/web/src/lib/liquidWaveStability.test.ts
//
// V5：60Hz 快速滑动 600 帧、双跳耦合 + 马赫尾迹持续叠加时的能量有界性。
// 目的不是"跑得动"，而是证明「解析耦合不会把能量灌爆」——
// 这是 U3 唯一的物理安全性判据：耦合是加性能量的，一旦漏掉限幅就是发散的。

import { describe, expect, it } from 'vitest';
import { createWaveField, injectWaveImpulse, stepWave, wavePeak } from './liquidWaveCore';
import { buildKelvinWakeSamples, COUPLING, planCoupling, type CouplingNode } from './liquidWaveGraph';

describe('V5: 高频双向激励 600 帧能量有界', () => {
  it('600 帧两跳耦合 + 尾迹叠加下，所有波场峰值严格 <= WAVE_MAX (1.5) 且确有可见能量', () => {
    const nodes = new Map<string, CouplingNode>([
      ['sec', { id: 'sec', layer: 'section', parentId: null, childIds: ['c1', 'c2'], slot: null }],
      [
        'c1',
        {
          id: 'c1',
          layer: 'card',
          parentId: 'sec',
          childIds: [],
          slot: { x0: 0.05, y0: 0.1, x1: 0.45, y1: 0.9 },
        },
      ],
      [
        'c2',
        {
          id: 'c2',
          layer: 'card',
          parentId: 'sec',
          childIds: [],
          slot: { x0: 0.55, y0: 0.1, x1: 0.95, y1: 0.9 },
        },
      ],
    ]);

    const fields = new Map([
      ['sec', createWaveField()],
      ['c1', createWaveField()],
      ['c2', createWaveField()],
    ]);
    const cooldown = new Map<string, number>();

    let maxObservedPeak = 0;
    for (let frame = 0; frame < 600; frame++) {
      const now = frame * 16.67;
      const nx = 0.2 + 0.6 * Math.abs(Math.sin(frame * 0.15));
      const ny = 0.5 + 0.3 * Math.cos(frame * 0.11);

      injectWaveImpulse(fields.get('c1')!, nx, ny, 0.9);
      for (const lobe of buildKelvinWakeSamples(nx, ny, 1, 0.2, 0.9)) {
        injectWaveImpulse(fields.get('c1')!, lobe.nx, lobe.ny, lobe.amp);
      }
      injectWaveImpulse(fields.get('sec')!, 0.05 + nx * 0.4, ny, 0.9 * 0.55);

      const hop1 = planCoupling(
        { sourceId: 'c1', nx, ny, unx: 1, uny: 0.2, baseAmp: 0.9, pointerCardId: 'c1', cooldown, now },
        nodes,
      );
      for (const inj of hop1) {
        injectWaveImpulse(fields.get(inj.targetId)!, inj.nx, inj.ny, inj.amp);
        cooldown.set(`c1>${inj.targetId}`, now);
      }

      const hop2 = planCoupling(
        { sourceId: 'sec', nx: 0.25, ny, unx: 1, uny: 0, baseAmp: 0.495, pointerCardId: 'c1', cooldown, now },
        nodes,
      );
      for (const inj of hop2) {
        injectWaveImpulse(fields.get(inj.targetId)!, inj.nx, inj.ny, inj.amp);
        cooldown.set(`sec>${inj.targetId}`, now);
      }

      for (const f of fields.values()) {
        stepWave(f);
        const p = wavePeak(f);
        if (p > maxObservedPeak) maxObservedPeak = p;
        expect(p).toBeLessThanOrEqual(1.5);
      }
    }

    // 有界不能等于"零能量"：必须确实起波，否则这条断言就是空跑。
    expect(maxObservedPeak).toBeGreaterThan(0.1);
    expect(COUPLING.maxAmp).toBeLessThanOrEqual(0.6);
  });
});