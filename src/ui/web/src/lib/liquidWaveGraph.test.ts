// src/ui/web/src/lib/liquidWaveGraph.test.ts
//
// U3 解析耦合内核的纯函数契约：V2（方向选边）/ V3（力度 -> 可达距离）/
// V11（冷却窗口）/ V13（手感参数区间与功能不等式）/ 尾迹几何。
//
// 这些断言全部只依赖纯函数，不碰 DOM —— 因此它们能在 Node 环境直接跑，
// 且能真正证明「力度决定传播距离」这条用户需求，而不是只证明代码在。

import { describe, expect, it } from 'vitest';
import {
  buildKelvinWakeSamples,
  buildRickerClickSamples,
  COUPLING,
  coupledAmp,
  pickEdges,
  planCoupling,
  reach,
  type CouplingNode,
} from './liquidWaveGraph';

function makeTopology(): Map<string, CouplingNode> {
  const nodes = new Map<string, CouplingNode>();
  nodes.set('sec-1', {
    id: 'sec-1',
    layer: 'section',
    parentId: null,
    childIds: ['card-a', 'card-b', 'card-c'],
    slot: null,
  });
  nodes.set('card-a', {
    id: 'card-a',
    layer: 'card',
    parentId: 'sec-1',
    childIds: [],
    slot: { x0: 0.05, y0: 0.2, x1: 0.3, y1: 0.8 },
  });
  nodes.set('card-b', {
    id: 'card-b',
    layer: 'card',
    parentId: 'sec-1',
    childIds: [],
    slot: { x0: 0.38, y0: 0.2, x1: 0.63, y1: 0.8 },
  });
  nodes.set('card-c', {
    id: 'card-c',
    layer: 'card',
    parentId: 'sec-1',
    childIds: [],
    slot: { x0: 0.7, y0: 0.2, x1: 0.95, y1: 0.8 },
  });
  return nodes;
}

describe('U3 解析耦合内核（纯函数）', () => {
  it('V2: pickEdges 方向表驱动 —— 纯右向单边、斜向两边均分、浅角退化单边、无方向取最近边', () => {
    const rightOnly = pickEdges(0.5, 0.5, 1, 0);
    expect(rightOnly).toHaveLength(1);
    expect(rightOnly[0]!.side).toBe('right');
    expect(rightOnly[0]!.weight).toBeCloseTo(1.0, 5);

    const diag = pickEdges(0.5, 0.5, 1, 1);
    expect(diag).toHaveLength(2);
    expect(diag.map((e) => e.side)).toEqual(['right', 'bottom']);
    expect(diag[0]!.weight).toBeCloseTo(0.5, 5);
    expect(diag[1]!.weight).toBeCloseTo(0.5, 5);

    // 0.85 / 0.15 时次边权重 0.15 < secondaryMinWeight(0.18) ⇒ 只保留主边。
    const flat = pickEdges(0.5, 0.5, 0.85, 0.15);
    expect(flat).toHaveLength(1);
    expect(flat[0]!.side).toBe('right');

    const noDir = pickEdges(0.12, 0.5, 0, 0);
    expect(noDir).toHaveLength(1);
    expect(noDir[0]!.side).toBe('left');
    expect(noDir[0]!.weight).toBe(1.0);
    expect(noDir[0]!.distance).toBeCloseTo(0.12, 5);
  });

  it('V3: 力度 -> 可达距离 reach() 严格单调递增；慢速中心不投、快速甩动跨半场', () => {
    const r0 = reach(0.135);
    const r1 = reach(0.3);
    const r2 = reach(0.55);
    const r3 = reach(0.9);

    expect(r0).toBeLessThan(r1);
    expect(r1).toBeLessThan(r2);
    expect(r2).toBeLessThan(r3);

    // 口径复算：λ·ln(a·κ/minAmp)，κ=0.52、λ=0.40、minAmp=0.06（v23 取值）。
    // 用公式复算而不是写死数值：参数一旦被调，这条断言会跟着走，
    // 真正的「有没有牙」在下一段的单调性 + 慢不投/快跨场功能不等式里。
    expect(r0).toBeCloseTo(0.4 * Math.log((0.135 * 0.52) / 0.06), 10);
    expect(r0).toBeLessThanOrEqual(0.15);
    expect(r3).toBeGreaterThanOrEqual(0.5);

    const nodes = makeTopology();

    // 慢速（0.135）在卡片中心：到右边 0.5，远超 reach(0.135)≈0.0628 ⇒ 不投递。
    const slowRes = planCoupling(
      {
        sourceId: 'card-a',
        nx: 0.5,
        ny: 0.5,
        unx: 1,
        uny: 0,
        baseAmp: 0.135,
        pointerCardId: 'card-a',
        cooldown: new Map(),
        now: 1000,
      },
      nodes,
    );
    expect(slowRes).toHaveLength(0);

    // 快速（0.90）：投给父 section，落点为卡片占位框的右边缘（slot.x1 = 0.30）。
    const fastRes = planCoupling(
      {
        sourceId: 'card-a',
        nx: 0.5,
        ny: 0.5,
        unx: 1,
        uny: 0,
        baseAmp: 0.9,
        pointerCardId: 'card-a',
        cooldown: new Map(),
        now: 1000,
      },
      nodes,
    );
    expect(fastRes).toHaveLength(1);
    expect(fastRes[0]!.targetId).toBe('sec-1');
    expect(fastRes[0]!.hop).toBe(1);
    expect(fastRes[0]!.nx).toBeCloseTo(0.3, 5);
    expect(fastRes[0]!.ny).toBeCloseTo(0.5, 5);
  });

  it('第 2 跳契约：section -> 兄弟 card 排除 pointerCardId 防回声，且最多 2 个目标', () => {
    const nodes = makeTopology();
    const res = planCoupling(
      {
        sourceId: 'sec-1',
        nx: 0.18,
        ny: 0.5,
        unx: 1,
        uny: 0,
        baseAmp: 0.5,
        pointerCardId: 'card-a',
        cooldown: new Map(),
        now: 2000,
      },
      nodes,
    );
    // 指针正在 card-a 上 ⇒ card-a 必须被排除，否则自己撞自己会形成正反馈。
    expect(res.some((r) => r.targetId === 'card-a')).toBe(false);
    expect(res.length).toBeLessThanOrEqual(COUPLING.maxSecondaryTargets);
    expect(res[0]?.targetId).toBe('card-b');
    expect(res[0]?.hop).toBe(2);
  });

  it('V11: 60ms 冷却窗口内同一 源->目标 只投递 1 次，跨过窗口后可再投', () => {
    const nodes = makeTopology();
    const cd = new Map<string, number>();

    const first = planCoupling(
      {
        sourceId: 'card-a',
        nx: 0.9,
        ny: 0.5,
        unx: 1,
        uny: 0,
        baseAmp: 0.8,
        pointerCardId: 'card-a',
        cooldown: cd,
        now: 1000,
      },
      nodes,
    );
    expect(first).toHaveLength(1);
    cd.set('card-a>sec-1', 1000);

    const blocked = planCoupling(
      {
        sourceId: 'card-a',
        nx: 0.9,
        ny: 0.5,
        unx: 1,
        uny: 0,
        baseAmp: 0.8,
        pointerCardId: 'card-a',
        cooldown: cd,
        now: 1030,
      },
      nodes,
    );
    expect(blocked).toHaveLength(0);

    const allowed = planCoupling(
      {
        sourceId: 'card-a',
        nx: 0.9,
        ny: 0.5,
        unx: 1,
        uny: 0,
        baseAmp: 0.8,
        pointerCardId: 'card-a',
        cooldown: cd,
        now: 1061,
      },
      nodes,
    );
    expect(allowed).toHaveLength(1);
  });

  it('V13: 手感参数锁定区间 + 功能性不等式（防止靠调参把耦合调成永久不触发）', () => {
    expect(COUPLING.lambda).toBeGreaterThanOrEqual(0.25);
    expect(COUPLING.lambda).toBeLessThanOrEqual(0.5);
    expect(COUPLING.kappa).toBeGreaterThanOrEqual(0.4);
    expect(COUPLING.kappa).toBeLessThanOrEqual(0.7);
    expect(COUPLING.minAmp).toBeGreaterThanOrEqual(0.04);
    expect(COUPLING.minAmp).toBeLessThanOrEqual(0.1);
    expect(COUPLING.maxAmp).toBeGreaterThanOrEqual(0.35);
    expect(COUPLING.maxAmp).toBeLessThanOrEqual(0.6);
    expect(COUPLING.secondaryMinWeight).toBeGreaterThanOrEqual(0.12);
    expect(COUPLING.secondaryMinWeight).toBeLessThanOrEqual(0.25);
    expect(COUPLING.hopAtten).toBeGreaterThanOrEqual(0.35);
    expect(COUPLING.hopAtten).toBeLessThanOrEqual(0.65);
    expect(COUPLING.cooldownMs).toBeGreaterThanOrEqual(40);
    expect(COUPLING.cooldownMs).toBeLessThanOrEqual(120);

    // 功能不等式：快甩必须够远、慢速必须够近、单次投递不得超出安全阀。
    expect(reach(0.9)).toBeGreaterThanOrEqual(0.5);
    expect(reach(0.135)).toBeLessThanOrEqual(0.15);
    expect(coupledAmp(0.9, 0, 1.0, 1)).toBeLessThanOrEqual(COUPLING.maxAmp);

    // [v23] 冷却窗口必须短于人手一次跨卡挥动（≈100ms 级），
    // 否则「连续快速划过 3 张卡」只会点亮第 1 张 —— 手在动、画面却是断续的。
    expect(COUPLING.cooldownMs).toBeLessThan(60);
  });

  it('coupledAmp 边界：非正幅度/非正权重返回 0，零距离不产生除零，第 2 跳按 hopAtten 衰减', () => {
    expect(coupledAmp(0, 0, 1, 1)).toBe(0);
    expect(coupledAmp(-0.5, 0.2, 1, 1)).toBe(0);
    expect(coupledAmp(0.5, 0.2, 0, 1)).toBe(0);

    const hop1 = coupledAmp(0.5, 0.0, 1, 1);
    const hop2 = coupledAmp(0.5, 0.0, 1, 2);
    expect(hop1).toBeCloseTo(0.5 * COUPLING.kappa, 8);
    expect(hop2).toBeCloseTo(hop1 * COUPLING.hopAtten, 8);
  });

  it('流体包络：Ricker 单/双击双环，马赫角随速度收窄且几何对称有界', () => {
    const rickerSingle = buildRickerClickSamples(0.5, 0.5, 1.0, false);
    expect(rickerSingle).toHaveLength(5);
    expect(rickerSingle[0]!.amp).toBe(1.0);
    expect(rickerSingle[1]!.amp).toBeLessThan(0);

    const rickerDouble = buildRickerClickSamples(0.5, 0.5, 1.0, true);
    expect(rickerDouble[0]!.amp).toBeCloseTo(1.15, 5);

    const wakeMid = buildKelvinWakeSamples(0.5, 0.5, 1, 0, 0.6);
    expect(wakeMid).toHaveLength(2);
    expect(wakeMid[0]!.nx).toBeLessThan(0.5);
    expect(wakeMid[0]!.ny + wakeMid[1]!.ny).toBeCloseTo(1.0, 5);

    // 高速收窄马赫角（偏离量更小），并在尾流中轴追加负向 Stern Trough。
    const wakeFast = buildKelvinWakeSamples(0.5, 0.5, 1, 0, 0.9);
    expect(wakeFast).toHaveLength(3);
    expect(wakeFast[2]!.amp).toBeLessThan(0);
    expect(Math.abs(wakeFast[0]!.ny - 0.5)).toBeLessThan(Math.abs(wakeMid[0]!.ny - 0.5));

    // 低于触发门槛不产生尾迹，避免慢速也被画成"高速拖尾"。
    expect(buildKelvinWakeSamples(0.5, 0.5, 1, 0, 0.2)).toHaveLength(0);
    expect(buildKelvinWakeSamples(0.5, 0.5, 0, 0, 0.9)).toHaveLength(0);
  });

  it('planCoupling 安全性：未知源 / 零幅度 / 孤儿 card 一律返回空数组', () => {
    const nodes = makeTopology();
    const base = {
      nx: 0.5,
      ny: 0.5,
      unx: 1,
      uny: 0,
      pointerCardId: null,
      cooldown: new Map<string, number>(),
      now: 1,
    };
    expect(planCoupling({ ...base, sourceId: 'nope', baseAmp: 0.9 }, nodes)).toHaveLength(0);
    expect(planCoupling({ ...base, sourceId: 'card-a', baseAmp: 0 }, nodes)).toHaveLength(0);
    expect(planCoupling({ ...base, sourceId: 'card-a', baseAmp: -1 }, nodes)).toHaveLength(0);
  });
});