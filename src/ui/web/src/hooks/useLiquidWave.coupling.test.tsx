// src/ui/web/src/hooks/useLiquidWave.coupling.test.ts
//
// U3 hook 级验收：V7（层级 z-index）/ V9（减弱动态）/ V10（空闲休眠与遥感归零）
// / V14（点击 Ricker 双环 + 跨层传导）/ 以及「波确实跨到了父底板与兄弟卡片」。
//
// 断言全部落在可观测副作用上（canvas 的 z-index / style、宿主上的 --rbc-impact-glow），
// 不做像素采样 —— 视觉采样属于真机验收（见 288 回执），单测里做像素断言只会自欺。

// @vitest-environment jsdom
import React, { useRef } from 'react';
import { cleanup, render } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { useLiquidWave } from './useLiquidWave';
import * as core from '../lib/liquidWaveCore';

let rafQueue: Array<{ id: number; cb: FrameRequestCallback }>;
let rafSeq: number;
let nowMs: number;

const stubMatchMedia = (reducedMotion: boolean) => {
  vi.stubGlobal('matchMedia', (query: string) => ({
    matches: reducedMotion && query.includes('prefers-reduced-motion'),
    media: query,
    onchange: null,
    addListener: () => {},
    removeListener: () => {},
    addEventListener: () => {},
    removeEventListener: () => {},
    dispatchEvent: () => false,
  }));
};

const pumpFrame = (t: number) => {
  const pending = rafQueue;
  rafQueue = [];
  for (const entry of pending) entry.cb(t);
};

const Harness: React.FC = () => {
  const rootRef = useRef<HTMLDivElement>(null);
  useLiquidWave({ root: rootRef });
  return (
    <div ref={rootRef}>
      <section className="rbc-section" data-testid="sec">
        <div className="rbc-card" data-testid="cardA" />
        <div className="rbc-card" data-testid="cardB" />
      </section>
    </div>
  );
};

function mockBox(el: HTMLElement, left: number, top: number, width: number, height: number) {
  Object.defineProperty(el, 'offsetLeft', { value: left, configurable: true });
  Object.defineProperty(el, 'offsetTop', { value: top, configurable: true });
  Object.defineProperty(el, 'offsetWidth', { value: width, configurable: true });
  Object.defineProperty(el, 'offsetHeight', { value: height, configurable: true });
  Object.defineProperty(el, 'clientWidth', { value: width, configurable: true });
  Object.defineProperty(el, 'clientHeight', { value: height, configurable: true });
  el.getBoundingClientRect = () =>
    ({
      left,
      top,
      right: left + width,
      bottom: top + height,
      width,
      height,
      x: left,
      y: top,
      toJSON: () => ({}),
    }) as DOMRect;
}

const glowOf = (el: HTMLElement): number =>
  parseFloat(el.style.getPropertyValue('--rbc-impact-glow') || '0');

const fire = (target: HTMLElement, type: string, x: number, y: number, detail = 1) => {
  target.dispatchEvent(
    new MouseEvent(type, { clientX: x, clientY: y, bubbles: true, detail, cancelable: true }),
  );
};

beforeEach(() => {
  rafQueue = [];
  rafSeq = 1;
  nowMs = 1000;
  stubMatchMedia(false);

  vi.stubGlobal('requestAnimationFrame', (cb: FrameRequestCallback) => {
    const id = rafSeq++;
    rafQueue.push({ id, cb });
    return id;
  });
  vi.stubGlobal('cancelAnimationFrame', (id: number) => {
    rafQueue = rafQueue.filter((e) => e.id !== id);
  });

  vi.spyOn(performance, 'now').mockImplementation(() => nowMs);
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockImplementation(
    () =>
      ({
        createImageData: (w: number, h: number) =>
          ({ data: new Uint8ClampedArray(w * h * 4), width: w, height: h }) as unknown as ImageData,
        putImageData: vi.fn(),
      }) as unknown as CanvasRenderingContext2D,
  );
});

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  document.documentElement.style.removeProperty('--rbc-impact-glow');
});

describe('useLiquidWave 跨层耦合（U3）', () => {
  it('V7: section 波场 z-index 为 0（沉在卡片下），card 波场 z-index 为 3（卡片最顶装饰层）', () => {
    const { getByTestId } = render(<Harness />);
    const secCanvas = getByTestId('sec').querySelector(
      ':scope > canvas.rbc-card-wave',
    ) as HTMLCanvasElement;
    const cardCanvas = getByTestId('cardA').querySelector(
      ':scope > canvas.rbc-card-wave',
    ) as HTMLCanvasElement;

    expect(secCanvas).not.toBeNull();
    expect(cardCanvas).not.toBeNull();
    // U2 两者都是 3 ⇒ section 波纹盖在卡片文字上（F24）。这里必须严格区分。
    expect(secCanvas.style.zIndex).toBe('0');
    expect(cardCanvas.style.zIndex).toBe('3');
  });

  it('波跨框传导：指针在卡片内移动时，父底板与兄弟卡片都被点亮（不是只在本卡片内自嗨）', () => {
    const { getByTestId } = render(<Harness />);
    const sec = getByTestId('sec');
    const cardA = getByTestId('cardA');
    const cardB = getByTestId('cardB');

    mockBox(sec, 0, 0, 800, 400);
    mockBox(cardA, 40, 50, 240, 300);
    mockBox(cardB, 360, 50, 240, 300);
    Object.defineProperty(cardA, 'offsetParent', { value: sec, configurable: true });
    Object.defineProperty(cardB, 'offsetParent', { value: sec, configurable: true });

    // 第一次落点建立速度基线（此时 lastX 为空 ⇒ speed=0，只打本地微光）。
    fire(cardA, 'pointermove', 120, 200);
    nowMs = 1016;
    // 向右快速滑动 130px / 16ms ⇒ 8125 px/s ⇒ 幅度打满 0.9，足以跨框。
    fire(cardA, 'pointermove', 250, 200);

    expect(glowOf(sec)).toBeGreaterThan(0);
    expect(glowOf(cardB)).toBeGreaterThan(0);
    // 底座与兄弟卡的撞击点必须被写下来，否则只有强度没有位置 = 一圈无源的光。
    expect(sec.style.getPropertyValue('--rbc-impact-x')).toMatch(/%$/);
    expect(cardB.style.getPropertyValue('--rbc-impact-y')).toMatch(/%$/);
  });

  it('慢速在卡片中心滑动不跨框：力度不足时耦合必须沉默（防止"永久联动"假生效）', () => {
    const { getByTestId } = render(<Harness />);
    const sec = getByTestId('sec');
    const cardA = getByTestId('cardA');
    const cardB = getByTestId('cardB');

    mockBox(sec, 0, 0, 800, 400);
    mockBox(cardA, 40, 50, 240, 300);
    mockBox(cardB, 360, 50, 240, 300);
    Object.defineProperty(cardA, 'offsetParent', { value: sec, configurable: true });
    Object.defineProperty(cardB, 'offsetParent', { value: sec, configurable: true });

    fire(cardA, 'pointermove', 155, 200);
    nowMs = 1016;
    // 中心附近只挪 2px / 16ms ⇒ 125 px/s ⇒ 幅度 0.15*0.9≈0.0945，
    // 到任一边界 >=0.5，远超 reach(0.0945) ⇒ 不投递。
    fire(cardA, 'pointermove', 157, 200);

    expect(glowOf(sec)).toBe(0);
    expect(glowOf(cardB)).toBe(0);
  });

  it('V14: 点击激发 Ricker 双环并向父底板传导，底板撞击点被点亮', () => {
    const spy = vi.spyOn(core, 'injectWaveImpulse');
    const { getByTestId } = render(<Harness />);
    const sec = getByTestId('sec');
    const cardA = getByTestId('cardA');

    mockBox(sec, 0, 0, 800, 400);
    mockBox(cardA, 40, 50, 240, 300);
    Object.defineProperty(cardA, 'offsetParent', { value: sec, configurable: true });

    spy.mockClear();
    fire(cardA, 'pointerdown', 268, 200);

    // 5 个 Ricker 采样（中心 + 四向反相环）+ 父底板主波 + 内侧毛细前驱 = 7。
    expect(spy.mock.calls.length).toBeGreaterThanOrEqual(7);
    expect(glowOf(sec)).toBeGreaterThan(0);
    expect(glowOf(cardA)).toBeGreaterThan(0);
  });

  it('V10: 联动结束、波真归静后画布休眠、will-change 释放、rAF 归零、遥测 awakeCount 归零', () => {
    const { getByTestId } = render(<Harness />);
    const sec = getByTestId('sec');
    const cardA = getByTestId('cardA');

    mockBox(sec, 0, 0, 800, 400);
    mockBox(cardA, 40, 50, 240, 300);
    Object.defineProperty(cardA, 'offsetParent', { value: sec, configurable: true });

    fire(cardA, 'pointerdown', 268, 200);
    const cardCanvas = cardA.querySelector<HTMLCanvasElement>('canvas.rbc-card-wave')!;
    expect(cardCanvas.style.willChange).toBe('opacity, contents');
    expect(rafQueue.length).toBeGreaterThan(0);

    // [v23] 判据从「超过 1200ms 就杀」改成「波真归静才杀」。
    // 只按时间杀会让低帧率机器上出现"波走一半突然消失"——正是用户第二条抱怨。
    // 现在必须同时满足：时间超窗（2200ms）**且** 残余峰值 < 0.02（强杀兜底）。
    // 点击波初始峰值 ~1.5，0.962 阻尼下要 ~160 帧才降到千分之二以下，
    // 所以这里推进 170 帧（2.7 s），让物理真的跑到归静。
    for (let i = 0; i < 170; i++) {
      nowMs += 16;
      pumpFrame(nowMs);
    }

    const secCanvas = sec.querySelector<HTMLCanvasElement>('canvas.rbc-card-wave')!;
    expect(cardCanvas.style.opacity).toBe('0');
    expect(cardCanvas.style.willChange).toBe('auto');
    expect(secCanvas.style.opacity).toBe('0');
    expect(rafQueue).toHaveLength(0);
    expect(window.__RBC_WAVE_PERF__?.awakeCount).toBe(0);
  });

  it('V15 遥测：slow frame 连续 30 帧后 hop2Disabled 置真（退化为单跳而非卡死）', () => {
    const { getByTestId } = render(<Harness />);
    const sec = getByTestId('sec');
    const cardA = getByTestId('cardA');
    mockBox(sec, 0, 0, 800, 400);
    mockBox(cardA, 40, 50, 240, 300);
    Object.defineProperty(cardA, 'offsetParent', { value: sec, configurable: true });

    fire(cardA, 'pointermove', 120, 200);

    // 让每帧耗时都超过 12ms 阈值：now 推进 20ms/帧，而 t0 在帧内取 nowMs 之前的值。
    let fake = 1000;
    vi.spyOn(performance, 'now').mockImplementation(() => {
      fake += 20;
      return fake;
    });

    for (let i = 0; i < 40; i++) {
      nowMs += 20;
      pumpFrame(nowMs);
    }

    expect(window.__RBC_WAVE_PERF__?.hop2Disabled).toBe(true);
  });

  it('V9: prefers-reduced-motion 命中时零 canvas、零注入、零 rAF', () => {
    stubMatchMedia(true);
    const spy = vi.spyOn(core, 'injectWaveImpulse');
    const { container, getByTestId } = render(<Harness />);

    expect(container.querySelectorAll('canvas.rbc-card-wave')).toHaveLength(0);
    fire(getByTestId('cardA'), 'pointermove', 120, 200);
    expect(spy).not.toHaveBeenCalled();
    expect(rafQueue).toHaveLength(0);
  });

  // [v23] 取消 pointerenter 边缘吸附的回归网。
  // 旧实现把入场点强行吸到 2% 的最近边（snapToNearestBorder），画面读起来是
  // 「卡片边缘凭空爆开一圈涟漪」，而不是「指针从哪儿进来、水就从哪儿凹下去」。
  // 这条缺陷在 289 之前没有任何判据（变异测试 M5 把吸附写回去，126 个用例全绿），
  // 所以必须补在 hook 级：断言注入点落在指针的真实位置，而不是任一条边。
  it('V18: pointerenter 在指针真实位置起波，不得吸附到容器边缘', () => {
    const { getByTestId } = render(<Harness />);
    const sec = getByTestId('sec');
    const cardA = getByTestId('cardA');
    mockBox(sec, 0, 0, 800, 400);
    mockBox(cardA, 40, 50, 240, 300);
    Object.defineProperty(cardA, 'offsetParent', { value: sec, configurable: true });

    const spy = vi.spyOn(core, 'injectWaveImpulse');
    spy.mockClear();

    // 卡片内 40,50 尺寸 240x300：点 (160, 200) 落在卡片正中 (nx=0.5, ny=0.5)。
    // 若被吸附到最近边，nx 或 ny 会变成 0.02 / 0.98 —— 这两个值很容易识别。
    fire(cardA, 'pointerenter', 160, 200);

    // 只取第一次注入（pointerenter 的本地主波），后面的尾迹/耦合调用不参与断言。
    const firstCall = spy.mock.calls[0];
    expect(firstCall).toBeDefined();
    const nx = firstCall![1] as number;
    const ny = firstCall![2] as number;

    expect(nx).toBeCloseTo(0.5, 1);
    expect(ny).toBeCloseTo(0.5, 1);
    // 反向守卫：不得出现"贴边"坐标
    expect(nx).not.toBe(0.02);
    expect(nx).not.toBe(0.98);
    expect(ny).not.toBe(0.02);
    expect(ny).not.toBe(0.98);
  });
});